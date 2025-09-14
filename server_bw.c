#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include <rdma/fi_errno.h>

#include <shared.h>
#include "benchmark_shared.h"

#include "shared.h"
#include "hmem.h"

extern size_t rx_buf_size;
extern size_t tx_buf_size;

int main(int argc, char **argv)
{
	int op, ret;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_BW;
	opts.options |= FT_OPT_SIZE;
	opts.window_size = 1;
	opts.src_port = "9999";
	opts.rma_op = FT_RMA_READ;
        opts.transfer_size = 100000;
        opts.iterations = 1;
        opts.warmup_iterations = 0;

	// Configure Hints
	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	hints->caps = FI_MSG | FI_RMA | FI_READ | FI_REMOTE_READ;
        hints->mode = FI_CONTEXT;
	hints->addr_format = FI_FORMAT_UNSPEC;
	hints->tx_attr->tclass = 0x203;
	hints->ep_attr->type = FI_EP_RDM;
	hints->domain_attr->threading = FI_THREAD_DOMAIN;
        hints->domain_attr->control_progress = FI_PROGRESS_UNSPEC;
        hints->domain_attr->data_progress = FI_PROGRESS_UNSPEC;
        hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_RAW | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT;
        hints->fabric_attr->prov_name = strdup("verbs;ofi_rxm");


	// Get Info
        uint64_t flags = FI_SOURCE;
        ret = fi_getinfo(FT_FIVERSION, opts.src_addr, opts.src_port, flags, hints, &fi);

        printf("IP/PORT/Flags: %s, %s, %" PRIu64 "\n\n", opts.src_addr, opts.src_port, flags);
        printf("Hints: %s\n\n", fi_tostr(hints,FI_TYPE_INFO));
        printf("Info: %s\n\n", fi_tostr(fi,FI_TYPE_INFO));

        ret = fi_fabric(fi->fabric_attr, &fabric, NULL);
        ret = fi_domain(fabric, fi, &domain, NULL);
	ret = fi_endpoint(domain, fi, &ep, NULL);

        // Open cq and av
        cq_attr.size = 2048;
	ret = fi_cq_open(domain, &cq_attr, &txcq, NULL);
	ret = fi_cq_open(domain, &cq_attr, &rxcq, NULL);
	av_attr.count = 1;
	ret = fi_av_open(domain, &av_attr, &av, NULL);
        if (ret) return ret;

	// Bind and enable
        fi_ep_bind(ep, &av->fid, 0);
	fi_ep_bind(ep, &txcq->fid, FI_TRANSMIT);
	fi_ep_bind(ep, &rxcq->fid, FI_RECV);
	ret = fi_enable(ep);

        if (ret) return ret;

	// Allocate buffer rx_buf tx_buf
	tx_size = rx_size = opts.transfer_size;
        tx_buf_size = rx_buf_size = tx_size + 32;

	tx_mr_size = 0;
	rx_mr_size = 0;

	buf_size = rx_buf_size + tx_buf_size + 64;
	buf = malloc(buf_size);
	memset(buf,1,buf_size);

	rx_buf = (char *)ft_get_aligned_addr(buf, 64);
	tx_buf = rx_buf + rx_buf_size;

	// Register buf
	mr = &no_mr;
	struct fi_mr_attr attr = {0};
        struct iovec iov = {0};
        iov.iov_base = buf;
        iov.iov_len = buf_size;

       	attr.mr_iov = &iov;
        attr.iov_count = 1;
        attr.access = FI_MR_RMA_EVENT | FI_MR_HMEM | FI_MR_COLLECTIVE; //ft_info_to_mr_access(fi);
        attr.offset = 0;
        attr.requested_key = FT_MR_KEY;
        attr.context = NULL;
        attr.iface = opts.iface;
        ret = fi_mr_regattr(domain, &attr, flags, &mr);
        mr_desc = fi_mr_desc(mr);

	if (ret) return ret;

        // fi_recv rx_buf
       	ret = fi_recv(ep, rx_buf, rx_size, mr_desc,  remote_fi_addr, &rx_ctx);
        struct fi_cq_err_entry comp;
        while(ret = fi_cq_read(rxcq, &comp, 1) == -FI_EAGAIN)
	{
	  // Code loops here until the client is ready?
	  // printf("fi_cq_read: %d", ret);
        }

        ret = ft_init_av();
        ret = ft_exchange_keys(&remote);

        // Write known values to rx/tx buffers.
	// Client uses rx=2, tx=4 so we can confirm we read from client
        memset(rx_buf, 3, opts.transfer_size);
        memset(tx_buf, 7, opts.transfer_size);

	// Do remote read
	// Note remote.addr + rx_buf_size = start address of tx buffer on client side
        while(ret = fi_read(ep, rx_buf, opts.transfer_size, mr_desc,
                            remote_fi_addr, remote.addr + rx_buf_size,
                            remote.key, &rx_ctx_arr[0].context) == -EAGAIN)
	{
              // printf("fi_read %d", ret);

              struct fi_cq_err_entry comp;
              while(ret = fi_cq_read(rxcq, &comp, 1) == -EAGAIN)
	      {
		 // printf("fi_cq_read: %d", ret);
	      }
        }


        printf("Data results: \n\n");
        for (int i = 0; i < 2*opts.transfer_size; i++) {
           if (i%100 == 0)
		   printf("%d ", buf[i]);
        }
	printf("\n\n");

	return -ret;
}
