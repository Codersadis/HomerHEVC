/*****************************************************************************
 * hmr_rate_control.c : homerHEVC encoding library
/*****************************************************************************
 * Copyright (C) 2014 homerHEVC project
 *
 * Juan Casal <jcasal.homer@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include "hmr_private.h"
#include "hmr_common.h"
#include <math.h>




void hmr_rc_init(hvenc_t* ed)
{
	ed->rc.vbv_size = ed->vbv_size*1000;
	ed->rc.vbv_fullness = ed->vbv_init*1000;
	ed->rc.average_pict_size = ed->bitrate*1000/ed->frame_rate;
	ed->rc.average_bits_per_ctu = ed->rc.average_pict_size/ed->pict_total_ctu;
}

void hmr_rc_init_seq(hvenc_t* ed)//seq goes from intra to intra
{
/*	ed->r = (int)floor(2.0*ed->bit_rate/ed->frame_rate + 0.5);//floor function returns a floating-point value representing the largest integer that is less than or equal to x

	// average activity 
	ed->avg_act = 1000;//pq el bitrate se mide en unidades de 400 bps
	ed->lastIntraFrame = -20;

	ed->d=(ed->vbv_buffer_size*512);//*1024 = vbv_buff
	ed->dAcc = 0;//(ed->vbv_buffer_size*256);
	ed->dAccRestar = 0;//ed->dAcc>>4;
*/
}


void hmr_rc_gop(hvenc_t* ed)//, int np, int nb)
{
//	ed->Np = ed->fieldpic ? 2*np+1 : np;
//	ed->Nb = ed->fieldpic ? 2*nb : nb;
}


void hmr_rc_change_pic_mode(henc_thread_t* et, slice_t *currslice)
{
	int ithreads;
	hvenc_t* ed = et->ed;
	if(ed->is_scene_change)
	{
		double pic_size_new = .5*ed->rc.average_pict_size*sqrt((double)ed->intra_period);	
		ed->rc.target_pict_size = pic_size_new;//.75*ed->rc.average_pict_size*sqrt((double)ed->intra_period);	

		ed->rc.target_bits_per_ctu = ed->rc.target_pict_size/ed->pict_total_ctu;

		for(ithreads=0;ithreads<ed->wfpp_num_threads;ithreads++)
		{
			henc_thread_t* henc_th = ed->thread[ithreads];
		
			henc_th->target_pict_size = (uint)ed->rc.target_pict_size;
		}
	}
}

void hmr_rc_init_pic(hvenc_t* ed, slice_t *currslice)
{
	int ithreads;


	switch(currslice->slice_type)
	{
	case  I_SLICE:
		currslice->qp = ed->pict_qp;
		ed->rc.target_pict_size = ed->rc.average_pict_size*sqrt((double)ed->intra_period);
		break;
	case  P_SLICE:
		currslice->qp = ed->pict_qp;
		ed->rc.target_pict_size = .75*ed->rc.average_pict_size;
		break;	
	case  B_SLICE:
		ed->rc.target_pict_size = ed->rc.average_pict_size/2;
		break;	
	}

#ifdef COMPUTE_AS_HM
	currslice->qp = ed->pict_qp-(ed->num_encoded_frames%4);
#endif

	ed->rc.target_bits_per_ctu = ed->rc.target_pict_size/ed->pict_total_ctu;

	for(ithreads=0;ithreads<ed->wfpp_num_threads;ithreads++)
	{
		henc_thread_t* henc_th = ed->thread[ithreads];
		
		henc_th->target_pict_size = (uint)ed->rc.target_pict_size;
		henc_th->num_encoded_ctus = 0;
		henc_th->num_bits = 0;
		henc_th->acc_qp = 0;
	}
}



void hmr_rc_end_pic(hvenc_t* ed, slice_t *currslice)
{
	double consumed_bitrate = 0.0;
	int consumed_ctus = 0;
	int avg_qp = 0;
	int ithreads;
	for(ithreads=0;ithreads<ed->wfpp_num_threads;ithreads++)
	{
		henc_thread_t* henc_th = ed->thread[ithreads];
		
		consumed_bitrate += henc_th->num_bits;
		consumed_ctus += henc_th->num_encoded_ctus;
		avg_qp+=henc_th->acc_qp;
	}

	if(ed->num_encoded_frames==21)
	{
		int iiiii=0;
	}


#ifndef COMPUTE_AS_HM
	avg_qp = (avg_qp+(consumed_ctus>>1))/consumed_ctus;
	ed->pict_qp = avg_qp;
#endif
	ed->rc.vbv_fullness += ed->rc.average_pict_size;
	

	if(currslice->slice_type == I_SLICE && ed->intra_period>1)
	{
//		ed->rc.vbv_fullness -= 1.*ed->rc.average_pict_size;
		ed->rc.acc_rate += consumed_bitrate - ed->rc.average_pict_size;
		ed->rc.acc_avg = ed->rc.acc_rate/ed->intra_period;
		consumed_bitrate = ed->rc.average_pict_size;
		ed->rc.vbv_fullness -= consumed_bitrate;
	}
/*	else if(consumed_bitrate>2.*ed->rc.average_pict_size)
	{
		ed->rc.acc_rate += consumed_bitrate - 2.*ed->rc.average_pict_size;
		ed->rc.acc_avg = ed->rc.acc_rate/ed->intra_period;
		consumed_bitrate = 2*ed->rc.average_pict_size;
		ed->rc.vbv_fullness -= consumed_bitrate;	
	}
*/	else
	{
		ed->rc.vbv_fullness -= consumed_bitrate;
		ed->rc.vbv_fullness -= ed->rc.acc_avg;
		ed->rc.acc_rate -= ed->rc.acc_avg;
	}

	if(ed->rc.vbv_fullness>ed->rc.vbv_size)
	{
		printf("HomerHEVC - vbv_overflow: efective bitrate is lower than expected\r\n");
		ed->rc.vbv_fullness>ed->rc.vbv_size;
	}

	if(ed->rc.vbv_fullness<0)
	{
		printf("HomerHEVC - vbv_underflow: efective bitrate is higher than expected\r\n");
		ed->rc.vbv_fullness>ed->rc.vbv_size;
	}
}


int hmr_rc_calc_cu_qp(henc_thread_t* curr_thread, ctu_info_t *ctu, cu_partition_info_t *curr_cu_info, slice_t *currslice)
{
	hvenc_t* ed = curr_thread->ed;
	int ithreads;
	double qp;
	double entropy_corrector;
	double pic_corrector = 0.0;
	double vbv_corrector;
	double consumed_bitrate = 0.0, entropy;
	double min_vbv_size;
	int consumed_ctus = 0.0;
	for(ithreads=0;ithreads<ed->wfpp_num_threads;ithreads++)
	{
		henc_thread_t* henc_th = ed->thread[ithreads];
		
		consumed_bitrate += henc_th->num_bits;
		consumed_ctus += henc_th->num_encoded_ctus;
	}

	entropy = sqrt(curr_cu_info->variance+.5*curr_cu_info->variance_chroma)/40;//25.0;
	if(consumed_ctus>0)
	{
		if(consumed_bitrate>1.5*ed->rc.target_bits_per_ctu)//*consumed_ctus && currslice->slice_type != I_SLICE)
			pic_corrector = .0125*(consumed_bitrate/(ed->rc.target_bits_per_ctu*consumed_ctus));
/*		if(ed->is_scene_change)
		{
			if(consumed_bitrate>1.*ed->rc.target_bits_per_ctu)//*consumed_ctus && currslice->slice_type != I_SLICE)
				pic_corrector = .0250*(consumed_bitrate/(ed->rc.target_bits_per_ctu*consumed_ctus));
		}
*/		

//		else if(currslice->slice_type == P_SLICE)
//		{
//			pic_corrector = -0.125*(consumed_bitrate/(ed->rc.target_bits_per_ctu*consumed_ctus));		
//		}
	}
	else
		pic_corrector = 0;

	if(ed->num_encoded_frames == 25)
	{
		int iiiii=0;
	}

	min_vbv_size = clip(ed->rc.vbv_fullness,ed->rc.vbv_fullness,ed->rc.vbv_size*.9);

	vbv_corrector = 1.0-clip((min_vbv_size-consumed_bitrate+ed->rc.target_bits_per_ctu*consumed_ctus)/ed->rc.vbv_size, 0.0, 1.0);

	qp = ((pic_corrector+vbv_corrector)/1.)*MAX_QP+/*(pic_corrector-1)+*/(entropy-2.);

	if(curr_thread->ed->intra_period>1)
	{
		if(currslice->slice_type == I_SLICE)
			qp/=1.25;
		if(ed->is_scene_change)
			qp/=1.05;
	}

	if((/*ctu->ctu_number<2 || */ed->is_scene_change) && qp<=5)
	{
		qp=5;
	}

	return (int)clip(qp+.5,/*MIN_QP*/1.0,MAX_QP);
}


int hmr_rc_get_cu_qp(henc_thread_t* et, ctu_info_t *ctu, cu_partition_info_t *curr_cu_info, slice_t *currslice)
{
	int qp;
#ifdef COMPUTE_AS_HM
	double debug_qp = 28+ctu->ctu_number%4;
	if(et->ed->bitrate_mode == BR_FIXED_QP)
	{
		qp = et->ed->current_pict.slice.qp;
	}
	else//cbr, vbr
	{
//		if(et->ed->qp_depth==0 )
//			qp = hmr_rc_calc_cu_qp(et);
/*		else */if(curr_cu_info->depth <= et->ed->qp_depth)
			qp = debug_qp;//hmr_rc_calc_cu_qp(et);
		else
			qp = curr_cu_info->parent->qp;
	}
#else
	if(et->ed->bitrate_mode == BR_FIXED_QP)
	{
		qp = et->ed->current_pict.slice.qp;
	}
	else//cbr, vbr
	{
//		if(et->ed->qp_depth==0 )
//			qp = hmr_rc_calc_cu_qp(et);
		if(curr_cu_info->depth <= et->ed->qp_depth)
		{
			qp = hmr_rc_calc_cu_qp(et, ctu, curr_cu_info, currslice);
			et->acc_qp+=qp;
		}
		else
			qp = curr_cu_info->parent->qp;
	}
#endif

	return qp;
}
