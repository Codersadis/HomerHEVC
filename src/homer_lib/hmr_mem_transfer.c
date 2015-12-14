/*****************************************************************************
 * hmr_mem_transfer.c : homerHEVC encoding library
/*****************************************************************************
 * Copyright (C) 2014 homerHEVC project
 *
 * Juan Casal <jcasal@homerhevc.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/

#include <math.h>
#include <memory.h>
#include <malloc.h>

#include "hmr_common.h"
#include "hmr_private.h"

#define ALIGNMENT 64

void *hmr_aligned_alloc(int num, int size)
{
    unsigned char *mem = (unsigned char *)malloc(num*size+ALIGNMENT+sizeof(void*));
    void **ptr = (void**)((uint64_t)(mem+ALIGNMENT+sizeof(void*)) & ~(ALIGNMENT-1));
    ptr[-1] = mem;
    return ptr;
}

void hmr_aligned_free(void *ptr) 
{
    free(((void**)ptr)[-1]);
}


#define WND_PADDING	
//Alignes by increasing offset_x and size_x to match the boundary. 
void wnd_alloc(wnd_t *wnd, int size_x, int size_y, int offset_x, int offset_y, int pix_size)
{
	int comp=0;
	//aligment - we want the boundaries of ctus alingned to 16 bytes for SSE4
	wnd->pix_size = pix_size;

	for(comp=0;comp<3;comp++)
	{
		int width = (comp==Y_COMP)?size_x:(size_x>>1);//420
		int height = (comp==Y_COMP)?size_y:(size_y>>1);//420
		int aligned_width = ((width*pix_size)%16)?(((width*pix_size)/16 + 1)*16):(width*pix_size);//((16-((size_x*pix_size)%16))&0xf);//right boundary - depends on chroma subsampling type
		int padding_size_x = (comp==Y_COMP)?(offset_x):(((offset_x)>>1)+(offset_x&0x1));//420
		int aligned_padding_x = ((padding_size_x*pix_size)%16)?(((padding_size_x*pix_size)/16 + 1)*16):(padding_size_x*pix_size);
		int padding_size_y = (comp==Y_COMP)?(offset_y):(((offset_y)>>1)+(offset_y&0x1));//420

		aligned_width = aligned_width/wnd->pix_size;
		aligned_padding_x = aligned_padding_x/wnd->pix_size;//Normalize

		wnd->window_size_x[comp] = aligned_padding_x+aligned_width+aligned_padding_x;
		wnd->window_size_y[comp] = padding_size_y+height+padding_size_y;

		if((wnd->palloc[comp] = (void*)hmr_aligned_alloc((wnd->window_size_x[comp]*wnd->window_size_y[comp])*pix_size+16, sizeof(byte)))==NULL)
			printf("wnd_alloc - unable to allocate memory for wnd->pwnd[%d]\r\n", comp);

		wnd->pwnd[comp]=(void*)((uint8_t*)wnd->palloc[comp]+((padding_size_y*wnd->window_size_x[comp]+aligned_padding_x)*wnd->pix_size));

		wnd->data_padding_x[comp] = padding_size_x;//aligned_padding_x;
		wnd->data_padding_y[comp] = padding_size_y;

		wnd->data_width[comp] = width;
		wnd->data_height[comp] = height;
	}
}

void wnd_delete(wnd_t *wnd)
{
	int i;
	for(i=0;i<3;i++)
	{
		if(wnd->palloc[i])//if(wnd->pwnd[i] != NULL)
			hmr_aligned_free(wnd->palloc[i]);
		wnd->palloc[i] = NULL;
		wnd->pwnd[i] = NULL;
	}
}


void wnd_realloc(wnd_t *wnd, int size_x, int size_y, int offset_x, int offset_y, int pix_size)
{
	wnd_delete(wnd);
	wnd_alloc(wnd, size_x, size_y, offset_x, offset_y, pix_size);
}

void wnd_copy(wnd_t * wnd_src, wnd_t * wnd_dst)
{
	int component;

	if(wnd_dst->data_width[0] != wnd_dst->data_width[0] || wnd_dst->data_height[0] != wnd_src->data_height[0])
	{
		printf("error: wnd_copy: windows of different size can not be coppied!!\r\n");
		return;
	}

	memcpy(wnd_dst->data_width, wnd_src->data_width, sizeof(wnd_src->data_width));
	memcpy(wnd_dst->data_height, wnd_src->data_height, sizeof(wnd_src->data_height));
	memcpy(wnd_dst->data_padding_x, wnd_src->data_padding_x, sizeof(wnd_src->data_padding_x));
	memcpy(wnd_dst->data_padding_y, wnd_src->data_padding_y, sizeof(wnd_src->data_padding_y));
	wnd_dst->pix_size = wnd_src->pix_size;

	for(component=Y_COMP;component<NUM_PICT_COMPONENTS;component++)
	{
		uint8_t* buff_src = WND_DATA_PTR(uint8_t*, *wnd_src, component);
		uint8_t* buff_dst = WND_DATA_PTR(uint8_t*, *wnd_dst, component);
		int src_stride =  WND_STRIDE_2D(*wnd_src, component);
		int dst_stride =  WND_STRIDE_2D(*wnd_dst, component);
		int height = WND_HEIGHT_2D(*wnd_src, component);
		int width = WND_WIDTH_2D(*wnd_src, component);
		int j;

		for(j=0;j<height;j++)
		{
			memcpy(buff_dst, buff_src, width*wnd_src->pix_size);
			buff_dst += wnd_src->pix_size*dst_stride;
			buff_src += wnd_src->pix_size*src_stride;
		}
	}
}

void wnd_copy_ctu(henc_thread_t* et, wnd_t * wnd_src, wnd_t * wnd_dst, ctu_info_t *ctu)
{
	int component;


	for(component=Y_COMP;component<NUM_PICT_COMPONENTS;component++)
	{
		int16_t *buff_src = WND_POSITION_2D(int16_t *, *wnd_src, component, ctu->x[component], ctu->y[component], 0, et->ctu_width);//WND_DATA_PTR(int16_t *, *wnd_src, component);
		int16_t *buff_dst = WND_POSITION_2D(int16_t *, *wnd_dst, component, ctu->x[component], ctu->y[component], 0, et->ctu_width);//WND_DATA_PTR(int16_t *, *wnd_src, component);
		int src_stride =  WND_STRIDE_2D(*wnd_src, component);
		int dst_stride =  WND_STRIDE_2D(*wnd_dst, component);
		int height = et->ctu_height[component];
		int width = et->ctu_width[component];
		int j;

		for(j=0;j<height;j++)
		{
			memcpy(buff_dst, buff_src, width*sizeof(buff_src[0]));
			buff_dst += dst_stride;
			buff_src += src_stride;
		}
	}
}


void wnd_zero_cu_1D(henc_thread_t* et, cu_partition_info_t* curr_part, wnd_t * wnd)
{
	int gcnt = 0;
	int j;//, i;
	int comp;

	for(comp=Y_COMP;comp<NUM_PICT_COMPONENTS;comp++)
	{
		int num_partitions = comp==Y_COMP?(curr_part->abs_index<<et->num_partitions_in_cu_shift):(curr_part->abs_index<<et->num_partitions_in_cu_shift)>>2;
		int size = (comp==Y_COMP)?curr_part->size:curr_part->size_chroma;
		int16_t * buff_dst = WND_POSITION_1D(int16_t  *, *wnd, comp, gcnt, et->ctu_width, num_partitions);

		for(j=0;j<size;j++)
		{
			memset(buff_dst, 0, size*sizeof(buff_dst[0]));
			buff_dst += size;
		}
	}
}

void wnd_copy_cu_2D(henc_thread_t* et, cu_partition_info_t* curr_part, wnd_t * wnd_src, wnd_t * wnd_dst)
{
	int gcnt = 0;
	int j;//, i;
	int comp;

	for(comp=Y_COMP;comp<NUM_PICT_COMPONENTS;comp++)
	{
		int size = (comp==Y_COMP)?curr_part->size:curr_part->size_chroma;
		int x_position = (comp==Y_COMP)?curr_part->x_position:curr_part->x_position_chroma;
		int y_position = (comp==Y_COMP)?curr_part->y_position:curr_part->y_position_chroma;
		int src_buff_stride = WND_STRIDE_2D(*wnd_src, comp);
		int dst_buff_stride = WND_STRIDE_2D(*wnd_dst, comp);
		int16_t * buff_src = WND_POSITION_2D(int16_t *, *wnd_src, comp, x_position, y_position, gcnt, et->ctu_width);
		int16_t * buff_dst = WND_POSITION_2D(int16_t *, *wnd_dst, comp, x_position, y_position, gcnt, et->ctu_width);

		for(j=0;j<size;j++)
		{
			memcpy(buff_dst, buff_src, size*sizeof(buff_src[0]));
			buff_src += src_buff_stride;
			buff_dst += dst_buff_stride;
		}
	}
}



//#ifdef WRITE_REF_FRAMES
void wnd_write2file(wnd_t *wnd, FILE *file)
{
	if(file==NULL)
		return;
	if(wnd->pix_size == 1)
	{
		byte * __restrict src;
		int stride, component, j;

		for(component=Y_COMP;component<=V_COMP;component++)//for(component=Y_COMP;component<=V_COMP;component++)
		{
			src = WND_DATA_PTR(byte*, *wnd, component);
			stride  = WND_STRIDE_2D(*wnd, component);
			for(j=0;j<wnd->data_height[component];j++)
			{
				fwrite(src, sizeof(byte), (wnd->data_width[component]), file); 
				src+=stride;
			}
			fflush(file);
		}
	}
	else if(wnd->pix_size == 2)
	{
		int16_t * src;
		int stride, component, i, j;

		for(component=Y_COMP;component<=V_COMP;component++)//for(component=Y_COMP;component<=V_COMP;component++)
		{
			src = WND_DATA_PTR(int16_t*, *wnd, component);
			stride  = WND_STRIDE_2D(*wnd, component);
			for(j=0;j<wnd->data_height[component];j++)
			{
				for(i=0;i<wnd->data_width[component];i++)
				{
					fwrite(&src[i], 1, sizeof(byte), file); 
				}
				src+=stride;
			}
			fflush(file);
		}
	}
}
//#endif


void mem_transfer_move_curr_ctu_group(henc_thread_t* et, int i, int j)//i,j are cu indexes
{
	int width, height, component;
	wnd_t* dst_wnd = &et->curr_mbs_wnd;
	byte * src;
	byte * dst;
	int src_stride, dst_stride;

	for(component=Y_COMP;component<=V_COMP;component++)
	{
		src = WND_POSITION_2D(int16_t *, et->enc_engine->current_pict.img2encode->img, component, (i*et->ctu_width[component]), (j*et->ctu_height[component]), 0, et->ctu_width);
		dst = WND_POSITION_2D(int16_t *, *dst_wnd, component, 0, 0, 0, et->ctu_width);

		src_stride =  WND_STRIDE_2D(et->enc_engine->current_pict.img2encode->img, component);
		dst_stride =  WND_STRIDE_2D(*dst_wnd, component);

		width = ((i+1)*et->ctu_width[component]<et->pict_width[component])?et->ctu_width[component]:(et->pict_width[component]-(i*et->ctu_width[component]));
		height = ((j+1)*et->ctu_height[component]<et->pict_height[component])?et->ctu_height[component]:(et->pict_height[component]-(j*et->ctu_height[component]));

		mem_transfer_2d2d(src, dst, width, height, src_stride, dst_stride);
	}
}


void mem_transfer_decoded_blocks(henc_thread_t* et, ctu_info_t* ctu)
{
	wnd_t *decoded_src_wnd = et->decoded_mbs_wnd[0];
	wnd_t *decoded_dst_wnd = &et->enc_engine->curr_reference_frame->img;
	int component = Y_COMP;
	int src_stride;
	int dst_stride;
	int16_t *decoded_buff_src;
	int16_t *decoded_buff_dst;
	int copy_width, copy_height, decoded_frame_width, decoded_frame_height;

	for(component=Y_COMP;component<=V_COMP;component++)
	{
		int j, i;
		decoded_buff_src = WND_POSITION_2D(int16_t *, *decoded_src_wnd, component, 0, 0, 0, et->ctu_width);
		decoded_buff_dst = WND_POSITION_2D(int16_t *, *decoded_dst_wnd, component, ctu->x[component], ctu->y[component], 0, et->ctu_width);
		src_stride =  WND_STRIDE_2D(*decoded_src_wnd, component);
		dst_stride =  WND_STRIDE_2D(*decoded_dst_wnd, component);

		decoded_frame_width = decoded_dst_wnd->data_width[component];
		decoded_frame_height = decoded_dst_wnd->data_height[component];
		copy_width = ((ctu->x[component]+et->ctu_width[component])<decoded_frame_width)?(et->ctu_width[component]):(decoded_frame_width-ctu->x[component]);
		copy_height = ((ctu->y[component]+et->ctu_height[component])<decoded_frame_height)?(et->ctu_height[component]):(decoded_frame_height-(ctu->y[component]));
//		mem_transfer_2d2d((uint8_t*)decoded_buff_src, (uint8_t*)decoded_buff_dst, copy_width*sizeof(decoded_buff_src[0]), copy_height, src_stride*sizeof(decoded_buff_src[0]), dst_stride*sizeof(decoded_buff_dst[0]));
		for(j=0;j<copy_height;j++)
		{
			for(i=0;i<copy_width;i++)
			{
				decoded_buff_dst[i] = decoded_buff_src[i];
			}
			decoded_buff_dst += dst_stride;
			decoded_buff_src += src_stride;
		}
	}
}


void mem_transfer_intra_refs(henc_thread_t* et, ctu_info_t* ctu)
{
	int l;
	wnd_t *decoded_src_wnd = &et->enc_engine->curr_reference_frame->img;
	wnd_t *decoded_dst_wnd = et->decoded_mbs_wnd[0];
	int component = Y_COMP;
	int src_stride = et->pict_width[component];
	int dst_stride;
	int16_t *decoded_buff_src;
	int16_t * decoded_buff_dst;
	int cu_size = ctu->size;
	int left_copy = 0, top_copy = 0;


	if(!ctu->ctu_left && !ctu->ctu_top)
		return;

	for(l=0;l<NUM_DECODED_WNDS;l++)
	{
		for(component=Y_COMP;component<=V_COMP;component++)
		{
			int i, j;
			decoded_dst_wnd = et->decoded_mbs_wnd[l];
			src_stride = WND_STRIDE_2D(*decoded_src_wnd, component);
			dst_stride = WND_STRIDE_2D(*decoded_dst_wnd, component);
			decoded_buff_src = WND_POSITION_2D(int16_t *, *decoded_src_wnd, component, ctu->x[component], ctu->y[component], 0, et->ctu_width);
			decoded_buff_dst = WND_POSITION_2D(int16_t *, *decoded_dst_wnd, component, 0, 0, 0, et->ctu_width);
			cu_size = et->ctu_width[component];
			left_copy = top_copy = 0;
			if(ctu->ctu_left)
				left_copy += cu_size;
			if(ctu->ctu_left_bottom)
				left_copy += cu_size;
			if(ctu->ctu_top)
				top_copy += cu_size;
			if(ctu->ctu_top_right)
				top_copy += cu_size;

			if(left_copy>(et->pict_height[component]-ctu->y[component]))
				left_copy=(et->pict_height[component]-ctu->y[component]);
			if(top_copy>(et->pict_width[component]-ctu->x[component]))
				top_copy=(et->pict_width[component]-ctu->x[component]);

			decoded_buff_src-=(src_stride+1);
			decoded_buff_dst-=(dst_stride+1);
	
			//top-left square
			if(ctu->ctu_left && ctu->ctu_top)
			{
				*decoded_buff_dst = *decoded_buff_src;
			}
			decoded_buff_src++;
			decoded_buff_dst++;
			//bottom line

			for(i=0;i<top_copy;i++)
				decoded_buff_dst[i] = decoded_buff_src[i];
			//memcpy(decoded_buff_dst, decoded_buff_src, top_copy*sizeof(decoded_buff_src[0]));

			//right column
			decoded_buff_src+=src_stride-1;
			decoded_buff_dst+=dst_stride-1;
			for(j=0;j<left_copy;j++)
			{
				decoded_buff_dst[j*dst_stride] = decoded_buff_src[j*src_stride];
			}
		}
	}
}


void mem_transfer_1d1d(unsigned char *src, unsigned char *dst, unsigned int width, unsigned int height)
{
	uint l;
	for(l=0;l<height*width;l+=16)
	{
#ifdef SSE_FUNCS_
		sse_128_store_vector_a(dst+l, sse_128_load_vector_u(src+l));
#else
		memcpy(dst,src,height*width);
#endif
	}		
}


void mem_transfer_1d2d(unsigned char *src, unsigned char *dst, unsigned int width, unsigned int height, unsigned int dst_stride)
{
	uint l;
	for(l=0;l<height;l++)
	{
#ifdef SSE_FUNCS_
		for(uint ll=0;ll<width;ll+=16)
		{
			sse_128_store_vector_a(dst+ll, sse_128_load_vector_u(src));
			src +=16;
		}
		dst  += dst_stride;
#else
		memcpy(dst,src,width);
		dst += dst_stride;
		src += width;
#endif
	}		
}



void mem_transfer_2d1d(unsigned char *src, unsigned char *dst, unsigned int width, unsigned int height, unsigned int src_stride)
{
	uint l;
	for(l=0;l<height;l++)
	{
#ifdef SSE_FUNCS_
		for(uint ll=0;ll<width;ll+=16)
		{
			sse_128_store_vector_u(dst+ll, sse_128_load_vector_u(src+ll));
			dst+=16;
		}
		src  += src_stride;
#else
		memcpy(dst,src,width);
		dst+=width;
		src  += src_stride;
#endif
	}
}

void mem_transfer_2d2d(unsigned char *src, unsigned char *dst, unsigned int width, unsigned int height, unsigned int src_stride, unsigned int dst_stride)
{
	uint l;
	for(l=0;l<height;l++)
	{
#ifdef SSE_FUNCS_
		for(uint ll=0;ll<width;ll+=16)
		{
			sse_128_store_vector_a(dst+ll, sse_128_load_vector_u(src+ll));
		}
		dst += dst_stride;
		src  += src_stride;
#else
		memcpy(dst,src,width);
		dst += dst_stride;
		src  += src_stride;
#endif
	}
}

