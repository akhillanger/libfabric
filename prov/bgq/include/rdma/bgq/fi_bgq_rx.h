/*
 * Copyright (C) 2016 by Argonne National Laboratory.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef _FI_PROV_BGQ_RX_H_
#define _FI_PROV_BGQ_RX_H_

#define FI_BGQ_UEPKT_BLOCKSIZE (1024)

/* forward declaration - see: prov/bgq/src/fi_bgq_atomic.c */
void fi_bgq_rx_atomic_dispatch (void * buf, void * addr, size_t nbytes,
	enum fi_datatype dt, enum fi_op op);

static inline
void dump_uepkt_queue (struct rx_operation * rx) {

	fprintf(stderr, "%s:%s():%d rx=%p, head=%p, tail=%p\n", __FILE__, __func__, __LINE__, rx, rx->ue.head, rx->ue.tail);
	struct fi_bgq_mu_packet * pkt = rx->ue.head;
	while (pkt) {
		fprintf(stderr, "%s:%s():%d --> %p\n", __FILE__, __func__, __LINE__, pkt);
		pkt = pkt->next;
	}
}

static inline
void complete_atomic_operation (struct fi_bgq_ep * bgq_ep, struct fi_bgq_mu_packet * pkt) {

	const uint32_t dt = pkt->hdr.atomic.dt;
	const uint32_t op = pkt->hdr.atomic.op;

	const uint64_t is_fetch = pkt->hdr.atomic.is_fetch;
	const uint64_t do_cntr = pkt->hdr.atomic.do_cntr;
	const uint64_t cntr_bat_id = pkt->hdr.atomic.cntr_bat_id;

	const uint16_t nbytes = pkt->hdr.atomic.nbytes_minus_1 + 1;

	const uint16_t key = pkt->hdr.atomic.key;
	const uint64_t offset = pkt->hdr.atomic.offset;
	const uintptr_t base = (uintptr_t)fi_bgq_domain_bat_read_vaddr(bgq_ep->rx.poll.bat, key);
	void * addr = (void*)(base + offset);

	const uint32_t origin = pkt->hdr.atomic.origin_raw & FI_BGQ_MUHWI_DESTINATION_MASK;

	if (is_fetch || (op == FI_ATOMIC_READ)) {

		const uint64_t dst_paddr = pkt->payload.atomic_fetch.metadata.dst_paddr;
		const uint64_t cq_paddr = pkt->payload.atomic_fetch.metadata.cq_paddr;
		const uint64_t fifo_map = pkt->payload.atomic_fetch.metadata.fifo_map;

		MUHWI_Descriptor_t * desc =
			fi_bgq_spi_injfifo_tail_wait(&bgq_ep->rx.poll.injfifo);

		qpx_memcpy64((void*)desc, (const void *)&bgq_ep->rx.poll.atomic_dput_model);

		desc->PacketHeader.NetworkHeader.pt2pt.Destination.Destination.Destination = origin;
		desc->Torus_FIFO_Map = fifo_map;

		/* locate the payload lookaside slot */
		uint64_t payload_paddr = 0;
		void * payload_vaddr =
			fi_bgq_spi_injfifo_immediate_payload(&bgq_ep->rx.poll.injfifo,
				desc, &payload_paddr);
		desc->Pa_Payload = payload_paddr;

		/* copy the target data into the injection lookaside buffer */
		memcpy(payload_vaddr, (const void*) addr, nbytes);
		desc->Message_Length = nbytes;

		MUSPI_SetRecPayloadBaseAddressInfo(desc, FI_BGQ_MU_BAT_ID_GLOBAL, dst_paddr);
		if (cq_paddr != 0) {	/* unlikely */
			desc->PacketHeader.messageUnitHeader.Packet_Types.Direct_Put.Counter_Offset =
				MUSPI_GetAtomicAddress(cq_paddr, MUHWI_ATOMIC_OPCODE_STORE_ADD);
		}

		fi_bgq_rx_atomic_dispatch((void*)&pkt->payload.atomic_fetch.data[0], addr, nbytes, dt, op);

		MUSPI_InjFifoAdvanceDesc(bgq_ep->rx.poll.injfifo.muspi_injfifo);

	} else {

		fi_bgq_rx_atomic_dispatch(&pkt->payload.byte[0], addr, nbytes, dt, op);

		/*
		 * cq completions (unlikely) are accomplished via a fence
		 * operation for non-fetch atomic operations
		 */
	}

	if (do_cntr) {	/* likely -- TODO: change to *always* do a counter update?? */

		const uint64_t is_local = pkt->hdr.atomic.is_local;

		MUHWI_Descriptor_t * desc =
			fi_bgq_spi_injfifo_tail_wait(&bgq_ep->rx.poll.injfifo);

		qpx_memcpy64((void*)desc, (const void*)&bgq_ep->rx.poll.atomic_cntr_update_model[is_local]);

		desc->PacketHeader.NetworkHeader.pt2pt.Destination.Destination.Destination = origin;
		desc->PacketHeader.messageUnitHeader.Packet_Types.Direct_Put.Rec_Payload_Base_Address_Id = cntr_bat_id;

		MUSPI_InjFifoAdvanceDesc(bgq_ep->rx.poll.injfifo.muspi_injfifo);
	}
}


/* The 'convert_batid_to_paddr' function assumes that the base+offset is a
 * virtual address, which then must be converted into a physical address in
 * FI_MR_SCALABLE mode.
 *
 * TODO - FI_MR_BASIC will use the base+offset as a physical addess
 */
static inline
void convert_batid_to_paddr (union fi_bgq_mu_descriptor * fi_mu_desc, struct fi_bgq_bat_entry * bat) {


	const uint8_t rma_update_type = fi_mu_desc->rma.update_type;

	if (rma_update_type == FI_BGQ_MU_DESCRIPTOR_UPDATE_BAT_TYPE_DST) {
		const uint64_t key_msb = fi_mu_desc->rma.key_msb;
		const uint64_t key_lsb = fi_mu_desc->rma.key_lsb;
		const uint64_t key = (key_msb << 48) | key_lsb;
		const uintptr_t base = (uintptr_t) fi_bgq_domain_bat_read_vaddr(bat, key);
		const uint64_t offset = fi_mu_desc->rma.dput.put_offset;

		uint64_t paddr = 0;
		fi_bgq_cnk_vaddr2paddr((const void *)(base+offset), 1, &paddr);
		MUSPI_SetRecPayloadBaseAddressInfo((MUHWI_Descriptor_t *)fi_mu_desc,
			FI_BGQ_MU_BAT_ID_GLOBAL, paddr);

	} else if (rma_update_type == FI_BGQ_MU_DESCRIPTOR_UPDATE_BAT_TYPE_SRC) {
		const uint64_t key_msb = fi_mu_desc->rma.key_msb;
		const uint64_t key_lsb = fi_mu_desc->rma.key_lsb;
		const uint64_t key = (key_msb << 48) | key_lsb;
		const uintptr_t base = (uintptr_t) fi_bgq_domain_bat_read_vaddr(bat, key);
		const uint64_t offset = fi_mu_desc->rma.Pa_Payload;

		fi_bgq_cnk_vaddr2paddr((const void *)(base+offset), 1, &fi_mu_desc->rma.Pa_Payload);
	} else {
		/* no update requested */
	}
}

static inline
void complete_rma_operation (struct fi_bgq_ep * bgq_ep, struct fi_bgq_mu_packet * pkt) {

	struct fi_bgq_bat_entry * bat = bgq_ep->rx.poll.bat;
	const uint64_t nbytes = pkt->hdr.rma.nbytes;
	const uint64_t ndesc = pkt->hdr.rma.ndesc;
	MUHWI_Descriptor_t * payload = (MUHWI_Descriptor_t *) &pkt->payload;

	if (nbytes > 0) {	/* only for direct-put emulation */

		uintptr_t vaddr = (uintptr_t)fi_bgq_domain_bat_read_vaddr(bat, pkt->hdr.rma.key);
		vaddr += pkt->hdr.rma.offset;

		const uint64_t payload_offset = ndesc << BGQ_MU_DESCRIPTOR_SIZE_IN_POWER_OF_2;
		memcpy((void*)vaddr, (void *)((uintptr_t)payload + payload_offset), nbytes);
	}

	/* The 'convert_batid_to_paddr' function assumes that the base+offset is
	 * a virtual address, which then must be converted into a physical
	 * address in FI_MR_SCALABLE mode.
	 *
	 * TODO - FI_MR_BASIC will use the base+offset as a physical addess
	 */
	assert(bgq_ep->domain->mr_mode == FI_MR_SCALABLE);	// rx->poll.domain->mr_mode

	unsigned i;
	for (i = 0; i < ndesc; ++i) {

		/*
		 * busy-wait until a fifo slot is available ..
		 */
		MUHWI_Descriptor_t * desc =
			fi_bgq_spi_injfifo_tail_wait(&bgq_ep->rx.poll.injfifo);

		qpx_memcpy64((void*)desc, (const void*)&payload[i]);

		if (desc->PacketHeader.NetworkHeader.pt2pt.Byte8.Packet_Type == 2) {	/* rget descriptor */

			/* locate the payload lookaside slot */
			uint64_t payload_paddr = 0;
			void * payload_vaddr =
				fi_bgq_spi_injfifo_immediate_payload(&bgq_ep->rx.poll.injfifo,
					desc, &payload_paddr);
			desc->Pa_Payload = payload_paddr;

			/* copy the rget payload descriptors into the injection lookaside buffer */
			union fi_bgq_mu_descriptor * rget_payload = (union fi_bgq_mu_descriptor *) payload_vaddr;
			qpx_memcpy64((void*)rget_payload, (const void*)&payload[i+1]);

			const uint64_t rget_ndesc = desc->Message_Length >> BGQ_MU_DESCRIPTOR_SIZE_IN_POWER_OF_2;
			i += rget_ndesc;

			unsigned j;
			for (j = 0; j < rget_ndesc; ++j) {
				convert_batid_to_paddr(&rget_payload[j], bat);
			}

		} else {

			convert_batid_to_paddr((union fi_bgq_mu_descriptor *)desc, bat);

		}
		MUSPI_InjFifoAdvanceDesc(bgq_ep->rx.poll.injfifo.muspi_injfifo);
	}
}


static inline
void inject_eager_completion (struct fi_bgq_ep * bgq_ep,
		struct fi_bgq_mu_packet * pkt) {

	const uint64_t is_local = pkt->hdr.send.is_local;
	const uint64_t cntr_paddr = ((uint64_t)pkt->hdr.send.cntr_paddr_rsh3b) << 3;

	MUHWI_Descriptor_t * desc =
		fi_bgq_spi_injfifo_tail_wait(&bgq_ep->rx.poll.injfifo);

	qpx_memcpy64((void*)desc, (const void*)&bgq_ep->rx.poll.ack_model[is_local]);

	MUSPI_SetRecPayloadBaseAddressInfo(desc, FI_BGQ_MU_BAT_ID_GLOBAL, cntr_paddr);
	desc->PacketHeader.NetworkHeader.pt2pt.Destination = pkt->hdr.send.origin;

	MUSPI_InjFifoAdvanceDesc(bgq_ep->rx.poll.injfifo.muspi_injfifo);

	return;
}


/**
 * \brief Complete a receive operation that has matched the packet header with
 * 		the match information
 *
 * \param[in]		bgq_ep	Edpoint associated with the receive
 * \param[in]		hdr	MU packet header that matched
 * \param[in,out]	entry	Completion entry
 */
static inline
void complete_receive_operation (struct fi_bgq_ep * bgq_ep,
		struct fi_bgq_mu_packet * pkt,
		const uint64_t origin_tag,
		union fi_bgq_context * context,
		const unsigned is_context_ext,
		const unsigned is_multi_receive,
		const unsigned is_manual_progress) {

	const uint64_t recv_len = context->len;
	void * recv_buf = context->buf;
	const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

	struct l2atomic_fifo_producer * err_producer = &bgq_ep->recv_cq->err_producer;

	if (packet_type & FI_BGQ_MU_PACKET_TYPE_INJECT) {

		const uint64_t send_len = pkt->hdr.inject.message_length;

		if (is_multi_receive) {		/* branch should compile out */
			if (send_len) memcpy(recv_buf, (void*)&pkt->hdr.inject.data, send_len);

			union fi_bgq_context * original_multi_recv_context = context;
			context = (union fi_bgq_context *)((uintptr_t)recv_buf - sizeof(union fi_bgq_context));
			assert((((uintptr_t)context) & 0x07) == 0);

			context->flags = FI_RECV | FI_MSG | FI_BGQ_CQ_CONTEXT_MULTIRECV;
			context->buf = recv_buf;
			context->len = send_len;
			context->byte_counter = 0;
			context->tag = 0;	/* tag is not valid for multi-receives */
			context->multi_recv_context = original_multi_recv_context;

			/* the next 'fi_bgq_context' must be 8-byte aligned */
			uint64_t bytes_consumed = ((send_len + 8) & (~0x07ull)) + sizeof(union fi_bgq_context);
			original_multi_recv_context->len -= bytes_consumed;
			original_multi_recv_context->buf = (void*)((uintptr_t)(original_multi_recv_context->buf) + bytes_consumed);

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}

		} else if (send_len <= recv_len) {
			if (send_len) memcpy(recv_buf, (void*)&pkt->hdr.inject.data, send_len);

			context->buf = NULL;
			context->len = send_len;
			context->byte_counter = 0;
			context->tag = origin_tag;

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}
		} else {	/* truncation - unlikely */
			struct fi_bgq_context_ext * ext;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
				ext->err_entry.op_context = ext->msg.op_context;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = FI_BGQ_CQ_CONTEXT_EXT;
				ext->err_entry.op_context = context;
			}

			ext->err_entry.flags = context->flags;
			ext->err_entry.len = recv_len;
			ext->err_entry.buf = recv_buf;
			ext->err_entry.data = 0;
			ext->err_entry.tag = origin_tag;
			ext->err_entry.olen = send_len - recv_len;
			ext->err_entry.err = FI_ETRUNC;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;

			ext->bgq_context.byte_counter = 0;

			/* post an 'error' completion event for the receive - spin loop! */
			uint64_t ext_rsh3b = (uint64_t)ext >> 3;
			while(0 != l2atomic_fifo_produce(err_producer, ext_rsh3b));
		}

		return;

	} else if (packet_type & FI_BGQ_MU_PACKET_TYPE_EAGER) {

		const uint64_t send_len = pkt->hdr.send.message_length;

		if (is_multi_receive) {		/* branch should compile out */
			if (send_len) memcpy(recv_buf, (void*)&pkt->payload.byte[0], send_len);

			union fi_bgq_context * original_multi_recv_context = context;
			context = (union fi_bgq_context *)((uintptr_t)recv_buf - sizeof(union fi_bgq_context));
			assert((((uintptr_t)context) & 0x07) == 0);

			context->flags = FI_RECV | FI_MSG | FI_BGQ_CQ_CONTEXT_MULTIRECV;
			context->buf = recv_buf;
			context->len = send_len;
			context->byte_counter = 0;
			context->tag = 0;	/* tag is not valid for multi-receives */
			context->multi_recv_context = original_multi_recv_context;

			/* the next 'fi_bgq_context' must be 8-byte aligned */
			uint64_t bytes_consumed = ((send_len + 8) & (~0x07ull)) + sizeof(union fi_bgq_context);
			original_multi_recv_context->len -= bytes_consumed;
			original_multi_recv_context->buf = (void*)((uintptr_t)(original_multi_recv_context->buf) + bytes_consumed);

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}

		} else if (send_len <= recv_len) {
			if (send_len) memcpy(recv_buf, (void*)&pkt->payload.byte[0], send_len);

			context->buf = NULL;
			context->len = send_len;
			context->byte_counter = 0;
			context->tag = origin_tag;

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}

		} else {	/* truncation - unlikely */
			struct fi_bgq_context_ext * ext;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
				ext->err_entry.op_context = ext->msg.op_context;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = FI_BGQ_CQ_CONTEXT_EXT;
				ext->err_entry.op_context = context;
			}

			ext->err_entry.flags = context->flags;
			ext->err_entry.len = recv_len;
			ext->err_entry.buf = recv_buf;
			ext->err_entry.data = 0;
			ext->err_entry.tag = origin_tag;
			ext->err_entry.olen = send_len - recv_len;
			ext->err_entry.err = FI_ETRUNC;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;

			ext->bgq_context.byte_counter = 0;

			/* post an 'error' completion event for the receive - spin loop! */
			uint64_t ext_rsh3b = (uint64_t)ext >> 3;
			while(0 != l2atomic_fifo_produce(err_producer, ext_rsh3b));
		}

		return;

	} else {			/* rendezvous packet */

		uint64_t niov = pkt->hdr.rendezvous.niov_minus_1 + 1;
		assert(niov <= (7-is_multi_receive));
		uint64_t xfer_len = pkt->payload.mu_iov[0].message_length;
		{
			uint64_t i;
			for (i=1; i<niov; ++i) xfer_len += pkt->payload.mu_iov[i].message_length;
		}

		uint64_t byte_counter_vaddr = 0;

		if (is_multi_receive) {		/* branch should compile out */

			union fi_bgq_context * multi_recv_context =
				(union fi_bgq_context *)((uintptr_t)recv_buf - sizeof(union fi_bgq_context));
			assert((((uintptr_t)multi_recv_context) & 0x07) == 0);

			multi_recv_context->flags = FI_RECV | FI_MSG | FI_BGQ_CQ_CONTEXT_MULTIRECV;
			multi_recv_context->buf = recv_buf;
			multi_recv_context->len = xfer_len;
			multi_recv_context->byte_counter = xfer_len;
			multi_recv_context->tag = 0;	/* tag is not valid for multi-receives */
			multi_recv_context->multi_recv_context = context;

			/* the next 'fi_bgq_context' must be 8-byte aligned */
			uint64_t bytes_consumed = ((xfer_len + 8) & (~0x07ull)) + sizeof(union fi_bgq_context);
			context->len -= bytes_consumed;
			context->buf = (void*)((uintptr_t)(context->buf) + bytes_consumed);

			byte_counter_vaddr = (uint64_t)&multi_recv_context->byte_counter;

			/* the original multi-receive context actually uses an
			 * operation counter - not a byte counter - but nevertheless
			 * the same field in the context structure is used */
			context->byte_counter += 1;

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}

		} else if (xfer_len <= recv_len) {

			context->len = xfer_len;
			context->byte_counter = xfer_len;
			context->tag = origin_tag;

			byte_counter_vaddr = (uint64_t)&context->byte_counter;

			/* post a completion event for the individual receive */
			if (is_manual_progress) {
				fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
			} else {
				struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
				uint64_t context_rsh3b = (uint64_t)context >> 3;
				while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
			}

		} else {
			/* truncation */
			struct fi_bgq_context_ext * ext;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
				ext->err_entry.op_context = ext->msg.op_context;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = FI_BGQ_CQ_CONTEXT_EXT;
				ext->err_entry.op_context = context;
			}

			ext->err_entry.flags = context->flags;
			ext->err_entry.len = recv_len;
			ext->err_entry.buf = recv_buf;
			ext->err_entry.data = 0;
			ext->err_entry.tag = origin_tag;
			ext->err_entry.olen = xfer_len - recv_len;
			ext->err_entry.err = FI_ETRUNC;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;

			ext->bgq_context.byte_counter = 0;

			byte_counter_vaddr = (uint64_t)&ext->bgq_context.byte_counter;

			/* post an 'error' completion event for the receive - spin loop! */
			uint64_t ext_rsh3b = (uint64_t)ext >> 3;
			while(0 != l2atomic_fifo_produce(err_producer, ext_rsh3b));

			xfer_len = 0;
			niov = 0;
		}

		/* determine the physical address of the byte counter memory */
		uint64_t byte_counter_paddr = 0;
		{
			Kernel_MemoryRegion_t mr;
			Kernel_CreateMemoryRegion(&mr, (void*)byte_counter_vaddr, sizeof(uint64_t));
			byte_counter_paddr = (uint64_t)mr.BasePa + (byte_counter_vaddr - (uint64_t)mr.BaseVa);
		}

		/* determine the physical address of the destination buffer */
		uint64_t dst_paddr = 0;
		{
			Kernel_MemoryRegion_t mr;
			Kernel_CreateMemoryRegion(&mr, (void*)recv_buf, recv_len);
			dst_paddr = (uint64_t)mr.BasePa + ((uint64_t)recv_buf - (uint64_t)mr.BaseVa);
		}

		const uint64_t is_local = pkt->hdr.rendezvous.is_local;

		/*
		 * inject a "remote get" descriptor - the payload is composed
		 * of two descriptors:
		 *
		 *   the first is a "direct put" descriptor that will rdma
		 *   transfer the source data from the origin and will
		 *   decrement a reception counter on the target as it
		 *   completes
		 *
		 *   the second is a "direct put" descriptor that will clear
		 *   the byte counter for the send completion entry on the
		 *   origin
		 */

		/* busy-wait until a fifo slot is available .. */
		MUHWI_Descriptor_t * rget_desc =
			fi_bgq_spi_injfifo_tail_wait(&bgq_ep->rx.poll.injfifo);

		assert(rget_desc);
		assert((((uintptr_t)rget_desc)&0x1F) == 0);

		/* locate the payload lookaside slot */
		uint64_t payload_paddr = 0;
		MUHWI_Descriptor_t * payload =
			(MUHWI_Descriptor_t *)fi_bgq_spi_injfifo_immediate_payload(&bgq_ep->rx.poll.injfifo,
				rget_desc, &payload_paddr);

		/* initialize the remote-get descriptor in the injection fifo */
		qpx_memcpy64((void*)rget_desc, (const void*)&bgq_ep->rx.poll.rzv.rget_model[is_local]);

		rget_desc->Pa_Payload = payload_paddr;
		rget_desc->PacketHeader.messageUnitHeader.Packet_Types.Remote_Get.Rget_Inj_FIFO_Id =
			pkt->hdr.rendezvous.rget_inj_fifo_id;	/* TODO - different rget inj fifos for tag vs msg operations? */

		fi_bgq_mu_packet_rendezvous_origin(pkt, &rget_desc->PacketHeader.NetworkHeader.pt2pt.Destination);

		/* initialize the direct-put ("data transfer") descriptor(s) in the rget payload */
		unsigned i;
		for (i=0; i<niov; ++i) {
			MUHWI_Descriptor_t * xfer_desc = payload++;

			qpx_memcpy64((void*)xfer_desc, (const void*)&bgq_ep->rx.poll.rzv.dput_model[is_local]);

			xfer_desc->Pa_Payload = pkt->payload.mu_iov[i].src_paddr;
			const uint64_t message_length = pkt->payload.mu_iov[i].message_length;
			xfer_desc->Message_Length = message_length;
			MUSPI_SetRecPayloadBaseAddressInfo(xfer_desc, FI_BGQ_MU_BAT_ID_GLOBAL, dst_paddr);
			dst_paddr += message_length;
			xfer_desc->PacketHeader.messageUnitHeader.Packet_Types.Direct_Put.Counter_Offset =
				MUSPI_GetAtomicAddress(byte_counter_paddr, MUHWI_ATOMIC_OPCODE_STORE_ADD);
			xfer_desc->PacketHeader.messageUnitHeader.Packet_Types.Direct_Put.Rec_Counter_Base_Address_Id =
				FI_BGQ_MU_BAT_ID_GLOBAL;

			rget_desc->Message_Length += sizeof(MUHWI_Descriptor_t);

			if (is_multi_receive) {		/* branch should compile out */
				if (is_local)
					xfer_desc->Torus_FIFO_Map = MUHWI_DESCRIPTOR_TORUS_FIFO_MAP_LOCAL0;
				else
					xfer_desc->Torus_FIFO_Map = fi_bgq_mu_packet_rendezvous_fifomap(pkt);
			}
		}

		/* initialize the direct-put ("origin completion") descriptor in the rget payload */
		{
			MUHWI_Descriptor_t * dput_desc = payload;
			qpx_memcpy64((void*)dput_desc, (const void*)&bgq_ep->rx.poll.rzv.dput_completion_model);

			const uint64_t counter_paddr = ((uint64_t) pkt->hdr.rendezvous.cntr_paddr_rsh3b) << 3;
			dput_desc->Pa_Payload =
				MUSPI_GetAtomicAddress(counter_paddr,
					MUHWI_ATOMIC_OPCODE_LOAD_CLEAR);
		}

		/* initialize the memory-fifo ("rendezvous ack") descriptor in the rget payload for multi-receives */
		if (is_multi_receive) {			/* branch should compile out */
			MUHWI_Descriptor_t * ack_desc = ++payload;
			qpx_memcpy64((void*)ack_desc, (const void*)&bgq_ep->rx.poll.rzv.multi_recv_ack_model);

			const uint64_t torus_fifo_map = is_local ? MUHWI_DESCRIPTOR_TORUS_FIFO_MAP_LOCAL0 : fi_bgq_mu_packet_rendezvous_fifomap(pkt);
			ack_desc->Torus_FIFO_Map = torus_fifo_map;
			rget_desc->Torus_FIFO_Map = torus_fifo_map;
			rget_desc->Message_Length += sizeof(MUHWI_Descriptor_t);

			union fi_bgq_mu_packet_hdr * hdr = (union fi_bgq_mu_packet_hdr *) &ack_desc->PacketHeader;
			hdr->ack.context = (uintptr_t) context;
		}

		/*
		 * inject the descriptor
		 */
		MUSPI_InjFifoAdvanceDesc(bgq_ep->rx.poll.injfifo.muspi_injfifo);
	}
	return;
}

static inline
unsigned is_match(struct fi_bgq_mu_packet *pkt, union fi_bgq_context * context, const unsigned poll_msg)
{
	if (poll_msg) return 1;		/* branch should compile out */

	const uint64_t origin_tag = pkt->hdr.inject.ofi_tag;
	const uint64_t ignore = context->ignore;
	const uint64_t target_tag = context->tag;
	const uint64_t target_tag_and_not_ignore = target_tag & ~ignore;
	const uint64_t origin_tag_and_not_ignore = origin_tag & ~ignore;

	return origin_tag_and_not_ignore == target_tag_and_not_ignore;
}


static inline
void process_rfifo_packet_optimized (struct fi_bgq_ep * bgq_ep, struct fi_bgq_mu_packet * pkt, const unsigned poll_msg, const unsigned is_manual_progress)
{
	const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

	if (poll_msg) {
		if (packet_type == FI_BGQ_MU_PACKET_TYPE_ACK) {	/* branch should compile out */

			union fi_bgq_context * context = (union fi_bgq_context *) pkt->hdr.ack.context;
			context->byte_counter -= 1;
			/* TODO - msync? */
			return;
		}

		if (packet_type == FI_BGQ_MU_PACKET_TYPE_RMA) {
			complete_rma_operation(bgq_ep, pkt);
			return;
		}

		if (packet_type == FI_BGQ_MU_PACKET_TYPE_ATOMIC) {
			complete_atomic_operation(bgq_ep, pkt);
			return;
		}
	}

	if ((packet_type & (FI_BGQ_MU_PACKET_TYPE_ACK|FI_BGQ_MU_PACKET_TYPE_EAGER)) ==
			(FI_BGQ_MU_PACKET_TYPE_ACK|FI_BGQ_MU_PACKET_TYPE_EAGER)) {	/* unlikely? */
		inject_eager_completion(bgq_ep, pkt);
	}

	assert(packet_type & (FI_BGQ_MU_PACKET_TYPE_INJECT | FI_BGQ_MU_PACKET_TYPE_EAGER | FI_BGQ_MU_PACKET_TYPE_RENDEZVOUS));

	/* search the match queue */
	union fi_bgq_context * head = bgq_ep->rx.poll.rfifo[poll_msg].mq.head;
	union fi_bgq_context * context = head;

	while (context) {

		const uint64_t rx_op_flags = context->flags;

		if (is_match(pkt, context, poll_msg)) {

			if (!poll_msg || ((rx_op_flags | FI_MULTI_RECV) == 0)) {	/* branch should compile out for tagged receives */

				union fi_bgq_context * next = context->next;
				union fi_bgq_context * prev = context->prev;

				/* remove the context from the match queue */
				if (prev) prev->next = next;
				else bgq_ep->rx.poll.rfifo[poll_msg].mq.head = next;

				if (next) next->prev = prev;
				else bgq_ep->rx.poll.rfifo[poll_msg].mq.tail = prev;

				const uint64_t is_context_ext = rx_op_flags & FI_BGQ_CQ_CONTEXT_EXT;

				/* branch will compile out */
				if (poll_msg)
					complete_receive_operation(bgq_ep, pkt,
						0, context, is_context_ext, 0, is_manual_progress);
				else
					complete_receive_operation(bgq_ep, pkt,
						pkt->hdr.inject.ofi_tag, context, is_context_ext, 0, is_manual_progress);

				return;

			} else {	/* FI_MULTI_RECV - unlikely */

				/* verify that there is enough space available in
				 * the multi-receive buffer for the incoming data */
				const uint64_t recv_len = context->len;
				uint64_t send_len = 0;

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_INJECT) {
					send_len = pkt->hdr.inject.message_length;
				} else if (packet_type & FI_BGQ_MU_PACKET_TYPE_EAGER) {
					send_len = pkt->hdr.send.message_length;
				} else /* FI_BGQ_MU_PACKET_TYPE_RENDEZVOUS */ {
					const uint64_t niov = pkt->hdr.rendezvous.niov_minus_1 + 1;
					send_len = pkt->payload.mu_iov[0].message_length;
					uint64_t i;
					for (i=1; i<niov; ++i) send_len += pkt->payload.mu_iov[i].message_length;
				}

				if (send_len > recv_len) {

					/* not enough space available in the multi-receive
					 * buffer; continue as if "a match was not found"
					 * and advance to the next match entry */
					context = context->next;

				} else {

					complete_receive_operation(bgq_ep, pkt,
						0, context, 0, 1, is_manual_progress);

					if (context->len < bgq_ep->rx.poll.min_multi_recv) {
						/* after processing this message there is not
						 * enough space available in the multi-receive
						 * buffer to receive the next message; post a
						 * 'FI_MULTI_RECV' event to the completion
						 * queue and return. */

						union fi_bgq_context * next = context->next;
						union fi_bgq_context * prev = context->prev;

						/* remove the context from the match queue */
						if (prev) prev->next = next;
						else bgq_ep->rx.poll.rfifo[poll_msg].mq.head = next;

						if (next) next->prev = prev;
						else bgq_ep->rx.poll.rfifo[poll_msg].mq.tail = prev;

						/* post a completion event for the multi-receive */
						if (is_manual_progress) {
							fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
						} else {
							struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
							uint64_t context_rsh3b = (uint64_t)context >> 3;
							while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
						}
					}
				}
				return;
			}

		} else {
			context = context->next;
		}
	}

	/* did not find a match .. add this packet to the unexpected queue */

	if (bgq_ep->rx.poll.rfifo[poll_msg].ue.free == NULL) { /* unlikely */
		struct fi_bgq_mu_packet * block = NULL;
		int rc = 0;
		rc = posix_memalign((void **)&block,
			32, sizeof(struct fi_bgq_mu_packet)*FI_BGQ_UEPKT_BLOCKSIZE);
		assert(rc==0);
		unsigned i;
		for (i=0; i<(FI_BGQ_UEPKT_BLOCKSIZE-1); ++i) block[i].next = &block[i+1];
		block[FI_BGQ_UEPKT_BLOCKSIZE-1].next = NULL;
		bgq_ep->rx.poll.rfifo[poll_msg].ue.free = block;
	}

	/* pop the free list */
	struct fi_bgq_mu_packet * uepkt = bgq_ep->rx.poll.rfifo[poll_msg].ue.free;
	bgq_ep->rx.poll.rfifo[poll_msg].ue.free = uepkt->next;

	/* copy the packet and append to the ue queue */
	size_t bytes_to_copy = (pkt->hdr.muhwi.NetworkHeader.pt2pt.Byte8.Size + 1) * 32;
	memcpy((void*)uepkt, (const void *)pkt, bytes_to_copy);
	uepkt->next = NULL;
	if (bgq_ep->rx.poll.rfifo[poll_msg].ue.head == NULL) {
		bgq_ep->rx.poll.rfifo[poll_msg].ue.head = uepkt;
	} else {
		bgq_ep->rx.poll.rfifo[poll_msg].ue.tail->next = uepkt;
	}
	bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = uepkt;

	return;
}

static inline
void process_rfifo_packet (struct fi_bgq_ep * bgq_ep, struct fi_bgq_mu_packet * pkt, const unsigned poll_msg, const unsigned is_manual_progress)
{
	process_rfifo_packet_optimized(bgq_ep, pkt, poll_msg, is_manual_progress);
}

static inline
int poll_rfifo (struct fi_bgq_ep * bgq_ep, const unsigned is_manual_progress) {

	/*
	 * The mu reception fifo is consumed by software at the 'head' and
	 * produced by hardware at the 'tail'.
	 */
	MUSPI_Fifo_t * fifo_ptr = &bgq_ep->rx.poll.muspi_recfifo->_fifo;
	assert(fifo_ptr);
	volatile uint64_t pa_tail = MUSPI_getHwTail(fifo_ptr);
	const uintptr_t pa_start = MUSPI_getStartPa(fifo_ptr);
	const uintptr_t offset_tail = pa_tail - pa_start;

	const uintptr_t va_head = (uintptr_t) MUSPI_getHeadVa(fifo_ptr);
	const uintptr_t va_start = (uintptr_t) MUSPI_getStartVa(fifo_ptr);
	const uintptr_t offset_head = va_head - va_start;

	MUHWI_PacketHeader_t * hdr = (MUHWI_PacketHeader_t *) va_head;

	if (offset_head < offset_tail) {			/* likely */

		muspi_dcbt(va_head, 0);
		_bgq_msync();

		const uintptr_t stop = va_head + offset_tail - offset_head;
		while ((uintptr_t)hdr < stop) {

			struct fi_bgq_mu_packet *pkt = (struct fi_bgq_mu_packet *) hdr;
			const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

			if (packet_type & FI_BGQ_MU_PACKET_TYPE_TAG) {	/* likely */
				process_rfifo_packet(bgq_ep, pkt, 0, is_manual_progress);
			} else {
				process_rfifo_packet(bgq_ep, pkt, 1, is_manual_progress);
			}

			hdr += hdr->NetworkHeader.pt2pt.Byte8.Size + 1;
			muspi_dcbt(hdr, 0);
		}

		MUSPI_setHeadVa(fifo_ptr, (void*)hdr);
		MUSPI_setHwHead(fifo_ptr, (uintptr_t)hdr-va_start);


	} else if (offset_head > offset_tail) {			/* unlikely ? */

		/* check if the head packet wraps */
		const uintptr_t va_end = (uintptr_t) fifo_ptr->va_end;
		if ((va_head + 544) < va_end) {			/* likely */

			/* head packet does not wrap */
			muspi_dcbt(va_head, 0);
			_bgq_msync();

			const uintptr_t stop = va_end - 544;
			while ((uintptr_t)hdr < stop) {

				struct fi_bgq_mu_packet *pkt = (struct fi_bgq_mu_packet *) hdr;
				const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_TAG) {	/* likely */
					process_rfifo_packet(bgq_ep, pkt, 0, is_manual_progress);
				} else {
					process_rfifo_packet(bgq_ep, pkt, 1, is_manual_progress);
				}

				hdr += hdr->NetworkHeader.pt2pt.Byte8.Size + 1;
				muspi_dcbt(hdr, 0);
			}

			MUSPI_setHeadVa(fifo_ptr, (void*)hdr);
			MUSPI_setHwHead(fifo_ptr, (uintptr_t)hdr-va_start);

		} else {					/* unlikely */

			/* head packet may wrap */
			muspi_dcbt(va_head, 0);
			_bgq_msync();

			uint32_t packet_bytes = ((uint32_t)hdr->NetworkHeader.pt2pt.Byte8.Size + 1) << 5;
			const uintptr_t bytes_before_wrap = va_end - va_head;
			if (packet_bytes < bytes_before_wrap) {
				struct fi_bgq_mu_packet *pkt = (struct fi_bgq_mu_packet *) hdr;
				const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_TAG) {	/* likely */
					process_rfifo_packet(bgq_ep, pkt, 0, is_manual_progress);
				} else {
					process_rfifo_packet(bgq_ep, pkt, 1, is_manual_progress);
				}

				const uintptr_t new_offset_head = offset_head + packet_bytes;
				MUSPI_setHeadVa(fifo_ptr, (void*)(va_start + new_offset_head));
				MUSPI_setHwHead(fifo_ptr, new_offset_head);

			} else if (packet_bytes == bytes_before_wrap) {
				struct fi_bgq_mu_packet *pkt = (struct fi_bgq_mu_packet *) hdr;
				const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_TAG) {	/* likely */
					process_rfifo_packet(bgq_ep, pkt, 0, is_manual_progress);
				} else {
					process_rfifo_packet(bgq_ep, pkt, 1, is_manual_progress);
				}

				MUSPI_setHeadVa(fifo_ptr, (void*)(va_start));
				MUSPI_setHwHead(fifo_ptr, 0);

			} else {
				uint8_t tmp_pkt[544] __attribute__((__aligned__(32)));

				memcpy((void*)&tmp_pkt[0], (void*)va_head, bytes_before_wrap);
				const uintptr_t bytes_after_wrap = packet_bytes - bytes_before_wrap;
				memcpy((void*)&tmp_pkt[bytes_before_wrap], (void*)va_start, bytes_after_wrap);

				hdr = (MUHWI_PacketHeader_t *)&tmp_pkt[0];
				struct fi_bgq_mu_packet *pkt = (struct fi_bgq_mu_packet *) hdr;
				const uint64_t packet_type = fi_bgq_mu_packet_type_get(pkt);

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_TAG) {	/* likely */
					process_rfifo_packet(bgq_ep, pkt, 0, is_manual_progress);
				} else {
					process_rfifo_packet(bgq_ep, pkt, 1, is_manual_progress);
				}

				MUSPI_setHeadVa(fifo_ptr, (void*)(va_start + bytes_after_wrap));
				MUSPI_setHwHead(fifo_ptr, bytes_after_wrap);
			}
		}
	}


	return 0;
}


/* rx_op_flags is only checked for FI_PEEK | FI_CLAIM | FI_MULTI_RECV
 * rx_op_flags is only used if FI_PEEK | FI_CLAIM | cancel_context
 * is_context_ext is only used if FI_PEEK | cancel_context | iovec
 *
 * The "normal" data movement functions, such as fi_[t]recv(), can safely
 * specify '0' for cancel_context, rx_op_flags, and is_context_ext, in
 * order to reduce code path.
 */
static inline
int process_mfifo_context (struct fi_bgq_ep * bgq_ep, const unsigned poll_msg,
		const uint64_t cancel_context, union fi_bgq_context * context,
		const uint64_t rx_op_flags, const uint64_t is_context_ext,
		const unsigned is_manual_progress) {

	if (cancel_context) {	/* branch should compile out */
		const uint64_t compare_context = is_context_ext ?
			(uint64_t)(((struct fi_bgq_context_ext *)context)->msg.op_context) :
			(uint64_t)context;

		if (compare_context == cancel_context) {

			struct fi_bgq_context_ext * ext;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = FI_BGQ_CQ_CONTEXT_EXT;
			}

			ext->bgq_context.byte_counter = 0;
			ext->err_entry.op_context = (void *)cancel_context;
			ext->err_entry.flags = rx_op_flags;
			ext->err_entry.len = 0;
			ext->err_entry.buf = 0;
			ext->err_entry.data = 0;
			ext->err_entry.tag = context->tag;
			ext->err_entry.olen = 0;
			ext->err_entry.err = FI_ECANCELED;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;

			/* post an 'error' completion event for the canceled receive - spin loop! */
			struct l2atomic_fifo_producer * producer =
				&bgq_ep->recv_cq->err_producer;
			while(0 != l2atomic_fifo_produce(producer, ((uint64_t)ext) >> 3));

			return FI_ECANCELED;
		}
	}

	if ((rx_op_flags & (FI_PEEK | FI_CLAIM | FI_MULTI_RECV)) == 0) {	/* likely */

		/* search the unexpected packet queue */
		struct fi_bgq_mu_packet * head = bgq_ep->rx.poll.rfifo[poll_msg].ue.head;
		struct fi_bgq_mu_packet * tail = bgq_ep->rx.poll.rfifo[poll_msg].ue.tail;
		struct fi_bgq_mu_packet * prev = NULL;
		struct fi_bgq_mu_packet * uepkt = head;

		unsigned found_match = 0;
		while (uepkt != NULL) {

			if (is_match(uepkt, context, poll_msg)) {

				/* branch will compile out */
				if (poll_msg)
					complete_receive_operation(bgq_ep, uepkt,
						0, context, 0, 0, is_manual_progress);
				else
					complete_receive_operation(bgq_ep, uepkt,
						uepkt->hdr.inject.ofi_tag, context, 0, 0, is_manual_progress);

				/* remove the uepkt from the ue queue */
				if (head == tail) {
					bgq_ep->rx.poll.rfifo[poll_msg].ue.head = NULL;
					bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = NULL;
				} else if (prev == NULL) {
					bgq_ep->rx.poll.rfifo[poll_msg].ue.head = uepkt->next;
				} else if (tail == uepkt) {
					bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = prev;
					prev->next = NULL;
				} else {
					prev->next = uepkt->next;
				}

				/* ... and prepend the uehdr to the ue free list. */
				uepkt->next = bgq_ep->rx.poll.rfifo[poll_msg].ue.free;
				bgq_ep->rx.poll.rfifo[poll_msg].ue.free = uepkt;

				/* found a match; break from the loop */
				uepkt = NULL;
				found_match = 1;

			} else {

				/* a match was not found; advance to the next ue header */
				prev = uepkt;
				uepkt = uepkt->next;
			}
		}

		if (!found_match) {

			/*
			 * no unexpected headers were matched; add this match
			 * information to the appropriate match queue
			 */

			union fi_bgq_context * tail = bgq_ep->rx.poll.rfifo[poll_msg].mq.tail;

			context->next = NULL;
			context->prev = tail;
			if (tail == NULL) {
				bgq_ep->rx.poll.rfifo[poll_msg].mq.head = context;
			} else {
				tail->next = context;
			}
			bgq_ep->rx.poll.rfifo[poll_msg].mq.tail = context;
		}

	} else if (rx_op_flags & FI_PEEK) {	/* unlikely */

		/* search the unexpected packet queue */
		struct fi_bgq_mu_packet * head = bgq_ep->rx.poll.rfifo[poll_msg].ue.head;
		struct fi_bgq_mu_packet * tail = bgq_ep->rx.poll.rfifo[poll_msg].ue.tail;
		struct fi_bgq_mu_packet * prev = NULL;
		struct fi_bgq_mu_packet * uepkt = head;

		unsigned found_match = 0;
		while (uepkt != NULL) {

			if (is_match(uepkt, context, poll_msg)) {

				const uint64_t packet_type = fi_bgq_mu_packet_type_get(uepkt);
				if (packet_type & FI_BGQ_MU_PACKET_TYPE_INJECT) {
					context->len = uepkt->hdr.inject.message_length;
				} else if (packet_type & FI_BGQ_MU_PACKET_TYPE_RENDEZVOUS) {
					const uint64_t niov = uepkt->hdr.rendezvous.niov_minus_1 + 1;
					uint64_t len = 0;
					unsigned i;
					for (i=0; i<niov; ++i) len += uepkt->payload.mu_iov[i].message_length;
					context->len = len;
				} else {	/* "eager" or "eager with completion" packet type */
					context->len = uepkt->hdr.send.message_length;
				}
				context->tag = poll_msg ? 0 : uepkt->hdr.inject.ofi_tag;
				context->byte_counter = 0;

				if (rx_op_flags & FI_CLAIM) { /* both FI_PEEK and FI_CLAIM were specified */
					assert((rx_op_flags & FI_BGQ_CQ_CONTEXT_EXT) == 0);

					context->claim = uepkt;

					/* remove the uepkt from the ue queue */
					if (head == tail) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.head = NULL;
						bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = NULL;
					} else if (prev == NULL) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.head = uepkt->next;
					} else if (tail == uepkt) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = prev;
						prev->next = NULL;
					} else {
						prev->next = uepkt->next;
					}
				}

				/* post a completion event for the receive */
				if (is_manual_progress) {
					fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
				} else {
					struct l2atomic_fifo_producer * producer =
						&bgq_ep->recv_cq->std_producer;
					const uint64_t mfifo_value = (uint64_t)context >> 3;
					while(0 != l2atomic_fifo_produce(producer, mfifo_value));	/* spin loop! */
				}

				found_match = 1;
				uepkt = NULL;

			} else {

				/* a match was not found; advance to the next ue header */
				prev = uepkt;
				uepkt = uepkt->next;
			}
		}

		if (!found_match) {

			/* did not find a match for this "peek" */

			struct fi_bgq_context_ext * ext;
			uint64_t mfifo_value;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
				mfifo_value = (uint64_t)context >> 3;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = rx_op_flags | FI_BGQ_CQ_CONTEXT_EXT;

				mfifo_value = (uint64_t)ext >> 3;
			}

			ext->err_entry.op_context = context;
			ext->err_entry.flags = rx_op_flags;
			ext->err_entry.len = 0;
			ext->err_entry.buf = 0;
			ext->err_entry.data = 0;
			ext->err_entry.tag = 0;
			ext->err_entry.olen = 0;
			ext->err_entry.err = FI_ENOMSG;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;
			ext->bgq_context.byte_counter = 0;

			/* post an 'error' completion event for the receive - spin loop! */
			struct l2atomic_fifo_producer * producer =
				&bgq_ep->recv_cq->err_producer;
			while(0 != l2atomic_fifo_produce(producer, mfifo_value));
		}

	} else if (rx_op_flags & FI_CLAIM) {	/* unlikely */
		assert((rx_op_flags & FI_BGQ_CQ_CONTEXT_EXT) == 0);

		/* only FI_CLAIM was specified
		 *
		 * this occurs after a previous FI_PEEK + FI_CLAIM
		 * operation has removed an unexpected packet from
		 * the queue and saved a pointer to it in the context
		 *
		 * complete the receive for this "claimed" message ... */
		struct fi_bgq_mu_packet * claimed_pkt = context->claim;
		if (poll_msg)
			complete_receive_operation(bgq_ep, claimed_pkt,
				0, context, 0, 0, is_manual_progress);
		else
			complete_receive_operation(bgq_ep, claimed_pkt,
				claimed_pkt->hdr.inject.ofi_tag, context, 0, 0, is_manual_progress);

		/* ... and prepend the uehdr to the ue free list. */
		claimed_pkt->next = bgq_ep->rx.poll.rfifo[poll_msg].ue.free;
		bgq_ep->rx.poll.rfifo[poll_msg].ue.free = claimed_pkt;

	} else if (poll_msg && (rx_op_flags & FI_MULTI_RECV)) {		/* unlikely - branch should compile out for tagged receives */

		/* search the unexpected packet queue */
		struct fi_bgq_mu_packet * head = bgq_ep->rx.poll.rfifo[poll_msg].ue.head;
		struct fi_bgq_mu_packet * tail = bgq_ep->rx.poll.rfifo[poll_msg].ue.tail;
		struct fi_bgq_mu_packet * prev = NULL;
		struct fi_bgq_mu_packet * uepkt = head;

		unsigned found_match = 0;
		while (uepkt != NULL) {

			if (is_match(uepkt, context, poll_msg)) {

				/* verify that there is enough space available in
				 * the multi-receive buffer for the incoming data */
				const uint64_t recv_len = context->len;
				const uint64_t packet_type = fi_bgq_mu_packet_type_get(uepkt);
				uint64_t send_len = 0;

				if (packet_type & FI_BGQ_MU_PACKET_TYPE_INJECT) {
					send_len = uepkt->hdr.inject.message_length;
				} else if (packet_type & FI_BGQ_MU_PACKET_TYPE_EAGER) {
					send_len = uepkt->hdr.send.message_length;
				} else if (packet_type & FI_BGQ_MU_PACKET_TYPE_RENDEZVOUS) {
					const uint64_t niov = uepkt->hdr.rendezvous.niov_minus_1 + 1;
					send_len = uepkt->payload.mu_iov[0].message_length;
					uint64_t i;
					for (i=1; i<niov; ++i) send_len += uepkt->payload.mu_iov[i].message_length;
				}

				if (send_len > recv_len) {

					/* not enough space available in the multi-receive
					 * buffer; continue as if "a match was not found"
					 * and advance to the next ue header */
					prev = uepkt;
					uepkt = uepkt->next;

				} else {
					complete_receive_operation(bgq_ep, uepkt,
						0, context, 0, 1, is_manual_progress);

					/* remove the uepkt from the ue queue */
					if (head == tail) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.head = NULL;
						bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = NULL;
					} else if (prev == NULL) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.head = uepkt->next;
					} else if (tail == uepkt) {
						bgq_ep->rx.poll.rfifo[poll_msg].ue.tail = prev;
						prev->next = NULL;
					} else {
						prev->next = uepkt->next;
					}

					/* ... and prepend the uehdr to the ue free list. */
					uepkt->next = bgq_ep->rx.poll.rfifo[poll_msg].ue.free;
					bgq_ep->rx.poll.rfifo[poll_msg].ue.free = uepkt;

					if (context->len < bgq_ep->rx.poll.min_multi_recv) {
						/* after processing this message there is not
						 * enough space available in the multi-receive
						 * buffer to receive the next message; break
						 * from the loop and post a 'FI_MULTI_RECV'
						 * event to the completion queue. */
						uepkt = NULL;
						found_match = 1;

						/* post a completion event for the multi-receive */
						if (is_manual_progress) {
							fi_bgq_cq_context_append(bgq_ep->recv_cq, context, 0);	/* TODO - IS lock required? */
						} else {
							uint64_t context_rsh3b = (uint64_t)context >> 3;
							struct l2atomic_fifo_producer * std_producer = &bgq_ep->recv_cq->std_producer;
							while(0 != l2atomic_fifo_produce(std_producer, context_rsh3b));	/* spin loop! */
						}
					}
				}

			} else {

				/* a match was not found; advance to the next ue header */
				prev = uepkt;
				uepkt = uepkt->next;
			}
		}

		if (!found_match) {

			/*
			 * no unexpected headers were matched; add this match
			 * information to the appropriate match queue
			 */

			union fi_bgq_context * tail = bgq_ep->rx.poll.rfifo[poll_msg].mq.tail;

			context->next = NULL;
			context->prev = tail;
			if (tail == NULL) {
				bgq_ep->rx.poll.rfifo[poll_msg].mq.head = context;
			} else {
				tail->next = context;
			}
			bgq_ep->rx.poll.rfifo[poll_msg].mq.tail = context;
		}
	}

	return 0;
}


static inline
int poll_mfifo (struct fi_bgq_ep * bgq_ep, const unsigned poll_msg, const uint64_t cancel_context, const unsigned is_manual_progress) {

#ifdef DEBUG
	if (bgq_ep->rx.poll.rfifo[poll_msg].ue.head == NULL) assert(bgq_ep->rx.poll.rfifo[poll_msg].ue.tail == NULL);
	if (bgq_ep->rx.poll.rfifo[poll_msg].ue.tail == NULL) assert(bgq_ep->rx.poll.rfifo[poll_msg].ue.head == NULL);
	if (bgq_ep->rx.poll.rfifo[poll_msg].mq.head == NULL) assert(bgq_ep->rx.poll.rfifo[poll_msg].mq.tail == NULL);
	if (bgq_ep->rx.poll.rfifo[poll_msg].mq.tail == NULL) assert(bgq_ep->rx.poll.rfifo[poll_msg].mq.head == NULL);
#endif

	/*
	 * attempt to match each new match element from the match fifo with any
	 * unexpected headers and compete the receives; if no match is found,
	 * append the match element to the match queue which will be searched
	 * for a match as each rfifo packet is processed
	 */
	uint64_t mfifo_value;
	struct l2atomic_fifo_consumer * consumer = &bgq_ep->rx.poll.rfifo[poll_msg].match;
	unsigned loop_count = 0;
	while (++loop_count < 16 && l2atomic_fifo_consume(consumer, &mfifo_value) == 0) {

		union fi_bgq_context * context = (union fi_bgq_context *)(mfifo_value << 3);
		const uint64_t rx_op_flags = context->flags;
		const uint64_t is_context_ext = rx_op_flags & FI_BGQ_CQ_CONTEXT_EXT;

		process_mfifo_context(bgq_ep, poll_msg, cancel_context,
			context, rx_op_flags, is_context_ext, is_manual_progress);

	}

	return 0;
}


static inline
int cancel_match_queue (struct fi_bgq_ep * bgq_ep, const unsigned poll_msg, const uint64_t cancel_context) {

	/* search the match queue */
	union fi_bgq_context * head = bgq_ep->rx.poll.rfifo[poll_msg].mq.head;
	union fi_bgq_context * tail = bgq_ep->rx.poll.rfifo[poll_msg].mq.tail;
	union fi_bgq_context * context = head;
	while (context) {

		const uint64_t is_context_ext = context->flags & FI_BGQ_CQ_CONTEXT_EXT;
		const uint64_t compare_context = is_context_ext ?
			(uint64_t)(((struct fi_bgq_context_ext *)context)->msg.op_context) :
			(uint64_t)context;

		if (compare_context == cancel_context) {

			/* remove the context from the match queue */
			if (context == head)
				bgq_ep->rx.poll.rfifo[poll_msg].mq.head = context->next;
			else
				context->prev->next = context->next;

			if (context == tail)
				bgq_ep->rx.poll.rfifo[poll_msg].mq.tail = context->prev;
			else
				context->next->prev = context->prev;

			struct fi_bgq_context_ext * ext;
			if (is_context_ext) {
				ext = (struct fi_bgq_context_ext *)context;
			} else {
				posix_memalign((void**)&ext, 32, sizeof(struct fi_bgq_context_ext));
				ext->bgq_context.flags = FI_BGQ_CQ_CONTEXT_EXT;
			}

			ext->bgq_context.byte_counter = 0;
			ext->err_entry.op_context = (void *)cancel_context;
			ext->err_entry.flags = context->flags;
			ext->err_entry.len = 0;
			ext->err_entry.buf = 0;
			ext->err_entry.data = 0;
			ext->err_entry.tag = context->tag;
			ext->err_entry.olen = 0;
			ext->err_entry.err = FI_ECANCELED;
			ext->err_entry.prov_errno = 0;
			ext->err_entry.err_data = NULL;

			/* post an 'error' completion event for the canceled receive - spin loop! */
			struct l2atomic_fifo_producer * producer =
				&bgq_ep->recv_cq->err_producer;
			while(0 != l2atomic_fifo_produce(producer, ((uint64_t)ext) >> 3));

			return FI_ECANCELED;
		}

		context = context->next;
	}

	return 0;
}

static inline
void poll_cfifo (struct fi_bgq_ep * bgq_ep, const unsigned is_manual_progress) {	/* TODO - make no inline */

	struct l2atomic_fifo_consumer * consumer = &bgq_ep->rx.poll.control;
	uint64_t value = 0;
	if (l2atomic_fifo_consume(consumer, &value) == 0) {

		const unsigned poll_fi_msg = bgq_ep->rx.caps & FI_MSG;
		const unsigned poll_fi_tag = bgq_ep->rx.caps & FI_TAGGED;

		/* const uint64_t flags = value & 0xE000000000000000ull; -- currently not used */
		const uint64_t cancel_context = value << 3;

		if (poll_fi_msg && poll_fi_tag) {
			if (FI_ECANCELED != cancel_match_queue(bgq_ep, 0, cancel_context)) {
				if (FI_ECANCELED != poll_mfifo(bgq_ep, 0, cancel_context, is_manual_progress)) {

					if (FI_ECANCELED != cancel_match_queue(bgq_ep, 1, cancel_context)) {
						if (FI_ECANCELED != poll_mfifo(bgq_ep, 1, cancel_context, is_manual_progress)) {
							/* did not find a match */
						}
					}
				}
			}
		} else if (poll_fi_msg) {
			if (FI_ECANCELED != cancel_match_queue(bgq_ep, 1, cancel_context)) {
				if (FI_ECANCELED != poll_mfifo(bgq_ep, 1, cancel_context, is_manual_progress)) {
					/* did not find a match */
				}
			}
		} else if (poll_fi_tag) {
			if (FI_ECANCELED != cancel_match_queue(bgq_ep, 0, cancel_context)) {
				if (FI_ECANCELED != poll_mfifo(bgq_ep, 0, cancel_context, is_manual_progress)) {
					/* did not find a match */
				}
			}
		}
	}
}

static inline
void poll_rx (struct fi_bgq_ep * bgq_ep,
		const unsigned poll_fi_msg,
		const unsigned poll_fi_tag) {

	volatile uint64_t * async_is_enabled = &bgq_ep->async.enabled;
	while (L2_AtomicLoad(async_is_enabled)) {
		unsigned loop_count = 64;
		do {
			if (poll_fi_msg) {
				poll_mfifo(bgq_ep, 1, 0, 0);
				poll_rfifo(bgq_ep, 0);
			}
			if (poll_fi_tag) {
				poll_mfifo(bgq_ep, 0, 0, 0);
				poll_rfifo(bgq_ep, 0);
			}
		} while (--loop_count);

		poll_cfifo(bgq_ep, 0);
	}
}

static inline
void * poll_fn (void *arg) {
//fprintf(stderr, "%s:%s():%d .... arg = %p\n", __FILE__, __func__, __LINE__, arg);
	struct fi_bgq_ep * bgq_ep = (struct fi_bgq_ep *) arg;

	volatile uint64_t * async_is_active = &bgq_ep->async.active;
	L2_AtomicStore(async_is_active, 1);

	uint64_t rx_caps = bgq_ep->rx.caps & (FI_MSG | FI_TAGGED);

	if (rx_caps == (FI_MSG | FI_TAGGED)) {
		poll_rx(bgq_ep, 1, 1);
	} else if (rx_caps == FI_MSG) {
		poll_rx(bgq_ep, 1, 0);
	} else if (rx_caps == FI_TAGGED) {
		poll_rx(bgq_ep, 0, 1);
	}

	L2_AtomicStore(async_is_active, 0);

	return NULL;
}



#endif /* _FI_PROV_BGQ_RX_H_ */
