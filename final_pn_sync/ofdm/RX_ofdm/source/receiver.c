#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include "prbs.h"
#include "ofdm.h"
#include "deque.h"
#include "kiss_fft.h"
#include "redpitaya/rp.h"
#define OFFLINE
#define N_FRAMES 70

uint64_t GetTimeStamp(){
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}

static complex_t sync_qam_tx[N_FFT] = {{0.0}};
static complex_t fft_in_buff[N_FFT] = {{0.0}};
static complex_t fft_out_buff[N_FFT] = {{0.0}};
static real_t rx_sig_buff[(N_FRAMES+1)*ADC_BUFFER_SIZE] = {0.0};
//static uint8_t gray_map[16] __attribute__((aligned(1))) = { 0, 1, 3, 2, 6, 7, 5, 4, 12, 13, 15, 14, 10, 11,  9,  8 };
static uint8_t gray_map[2] __attribute__((aligned(1))) = { 0, 1};

static inline real_t comp_mag_sqr( complex_t num ){
    return (num.r*num.r + num.i*num.i);
}

static inline complex_t comp_div( complex_t num, complex_t den){
    complex_t div;
    div.r = (num.r*den.r + num.i*den.i)/comp_mag_sqr(den);
    div.i = (num.i*den.r - num.r*den.i)/comp_mag_sqr(den);
    return div;
}

void qam_mod ( complex_t *qam_data, uint8_t *bin_data, uint8_t is_sync_sym)
{
    complex_t temp, *head_ptr, *tail_ptr;
    // temp variable for real and imag components of the QAM symbol
    uint32_t j, i, qam_r, qam_i;
    // max integer value of the real/imag component of the QAM symbol
    real_t qam_limit = pow(2, N_BITS/2) - 1;
    // Nomalization constant for getting average unit power per ofdm symbol
    real_t norm_const = 1/sqrt( 16*2*(M_QAM - 1)/3*N_DSC/(1+is_sync_sym) );

    // get the header and tail pointers of the output buffer
    head_ptr = qam_data + 1;
    tail_ptr = qam_data + N_FFT - 1;

    for(i=0; i<N_QAM; i++)
    {
        // reset real and imag parts
        qam_r = 0;
        qam_i = 0;

        // separate the real and imag bits and convert to decimal for gray coding
        for (j=1; j<=N_BITS/2; j++){
            qam_r |= ( (*(bin_data + N_BITS/2 - j) ) << (j-1) );
            qam_i |= ( (*(bin_data + N_BITS - j) ) << (j-1) );
        }

        temp.r =  (2*gray_map[qam_r] -qam_limit)*norm_const;
        temp.i = -(2*gray_map[qam_i] -qam_limit)*norm_const;

        // advance the bin pointer to next symbol
        bin_data += N_BITS;

        // sync symbols only used odd data subcarriers
        (head_ptr)->r = temp.r;
        (head_ptr++)->i = temp.i;
        (head_ptr)->r = 0.0;
        (head_ptr++)->i = 0.0;
        (tail_ptr)->r = temp.r;
        (tail_ptr--)->i = -temp.i;
        (tail_ptr)->r = 0.0;
        (tail_ptr--)->i = -0.0;
        i++;
    }
}

void generate_ofdm_sync(){

    // sync sequence binary data holder
    uint8_t *sync_bin = malloc(N_DSC*sizeof(uint8_t)/2);
    // generate binary data for sync sequence
    pattern_LFSR_byte(PRBS7, sync_bin, N_DSC/2);
    // qam modulate the binary data: [N_FFT] = [DC QAM_DATA (N_GAURD/2-1) conj(flip(QAM_DATA))]
    qam_mod( sync_qam_tx, sync_bin, TRUE);
}

void qam_demod(uint8_t *bin_data, complex_t *qam_data){

	complex_t temp;
	uint32_t i, j, qam_sym_r, qam_sym_i;
	real_t qam_limit = pow(2, N_BITS/2) - 1;
	real_t norm_const = sqrt( 16*(2*(M_QAM - 1)/3)*N_DSC)/N_FFT;

	for(i=0; i<N_QAM; i++)
	{
		//extract the qam data from input buffer
	  	temp.r = qam_data->r*norm_const;
	  	temp.i = -qam_data->i*norm_const;

		// Hard Limit Data Within QAM Symbol Boundaries
		if (temp.r > qam_limit)
			temp.r	= qam_limit;
		else if (temp.r < -qam_limit)
			temp.r	= -qam_limit;
		if (temp.i > qam_limit)
			temp.i	= qam_limit;
		else if (temp.i < -qam_limit)
			temp.i	= -qam_limit;

		// Demodulate based on minimum distance from constellation points
		qam_sym_r = gray_map[ (uint8_t)(round((temp.r+qam_limit)/2)) ];
		qam_sym_i = gray_map[ (uint8_t)(round((temp.i+qam_limit)/2)) ];

		//write the binary data into output buffer
		for (j=1; j<=N_BITS/2; j++){
			(*(bin_data + N_BITS/2 - j)) = (qam_sym_r>>(j-1))&0x1;
			(*(bin_data + N_BITS - j)) = (qam_sym_i>>(j-1))&0x1;
		}
		bin_data += N_BITS;
		qam_data++;
    }
}

uint32_t ofdm_demod(uint8_t *bin_rx, uint32_t start_idx, uint32_t stop_idx, uint32_t *bits_recvd){

	complex_t *fft_out = fft_out_buff, *fft_in = fft_in_buff;
    complex_t ht[N_FFT] = {{0.0}}, hf[N_FFT] = {{0.0}};
	kiss_fft_cfg fft_cfg = kiss_fft_alloc(N_FFT, FALSE, NULL, NULL);
	kiss_fft_cfg ifft_cfg = kiss_fft_alloc(N_FFT, TRUE, NULL, NULL);
    const int32_t max_sync_error = floor(N_CP_SYNC/2), n_cp_rem = (N_CP_SYNC - max_sync_error);
    const int32_t corr_len = OSF*N_FFT/2/PRE_DSF, win_len = POST_DSF*N_CP_SYNC, corr_th = POST_DSF*1.5;
    static int32_t corr_count = -1, sym_count = 0, sync_corrected  = 0, sync_done= 0, sync_idx=0;
    static real_t max_of_min = 0.0, cros_corr_s=0.0, auto_corr_s=0.0, auto_corr=0.0, cros_corr=0.0, corr_fact = 0.0;
    int32_t base_idx=0, half_idx=0, full_idx=0, sync_err = N_FFT/4;
    static dequeue window;

    // Cross Corr: [0:NFFT/2-1]*[NFFT/2:NFFT-1], Auto Corr: [NFFT/2:NFFT-1]*[NFFT/2:NFFT-1]
    // Base index = 0 of first half, Half Index = 0 of second half, Full Index = 0 of third Half
    base_idx = start_idx;
    half_idx = start_idx+N_FFT*OSF/2;
    full_idx = start_idx+N_FFT*OSF;

    fprintf(stdout,"Sync/Demod started, demod_idx = %d, revcd_idx = %d\n", start_idx, stop_idx);

    if(!sync_done){
        if(corr_count<0){
            sym_count = 0;
            initialize(&window);
            if(stop_idx > full_idx ){
                for (int i=0; i<corr_len; i++){
                    auto_corr += rx_sig_buff[(half_idx+i*PRE_DSF) ]*rx_sig_buff[(half_idx+i*PRE_DSF) ];
                    cros_corr += rx_sig_buff[(base_idx+i*PRE_DSF) ]*rx_sig_buff[(half_idx+i*PRE_DSF) ];
                }
                corr_fact = fabs(cros_corr/auto_corr);
                corr_count++;
            } else{
                stop_idx = start_idx;
                fprintf(stdout,"Sync not completed samples not enough[%d], stop_idx = %d\n", OSF*N_FFT, stop_idx);
            }
        }
        if(corr_count>=0){
            while( stop_idx > full_idx) {
                while( !empty(&window) && window.data[window.rear].value >= corr_fact)
                    dequeueR(&window);
                enqueueR( &window, (pair){.value = corr_fact, .position = base_idx} );
                while( (window.data[window.front].position + win_len) < base_idx )
                    dequeueF(&window);
                if( window.data[window.front].value > max_of_min  && corr_count >= win_len ){
                    max_of_min = window.data[window.front].value;
                    sync_idx = base_idx - win_len*PRE_DSF;
                    auto_corr_s = auto_corr;
                    cros_corr_s = fabs(cros_corr);
                } else if ( window.data[window.front].value < 0.8*max_of_min && cros_corr_s > corr_th && max_of_min > 0.7){
                    sync_done = 1;
                    break;
                }
                auto_corr = auto_corr + rx_sig_buff[full_idx]*rx_sig_buff[full_idx] - rx_sig_buff[half_idx]*rx_sig_buff[half_idx];
                cros_corr = cros_corr + rx_sig_buff[half_idx]*rx_sig_buff[full_idx] - rx_sig_buff[base_idx]*rx_sig_buff[half_idx];
                corr_fact = fabs(cros_corr/auto_corr);
                corr_count++;
                base_idx += PRE_DSF;
                half_idx += PRE_DSF;
                full_idx += PRE_DSF;
            }
            if(sync_done){
                fprintf(stdout,"Sync completed, sync_idx = %d, corr_count = %d\n", sync_idx, corr_count);
                fprintf(stdout,"Sync completed, max of min = %f, auto corr = %f, cross_corr=%f\n", max_of_min, auto_corr_s, cros_corr_s);
            } else{
                stop_idx = full_idx;
                fprintf(stdout,"Sync not completed, stop_idx = %d, corr_count = %d\n", stop_idx, corr_count);
                fprintf(stdout,"Sync not completed, max of min = %f, auto corr = %f, cross_corr=%f\n", max_of_min, auto_corr_s, cros_corr_s);
            }
        }
    }
    if(sync_done){
        if (!sync_corrected){
            if ( stop_idx > (start_idx + SYNC_SYM_LEN) ){

                // collect samples for pilot estimation
    		    for( int i=0; i< N_FFT; i++)
		    	    fft_in[i].r = rx_sig_buff[start_idx + (n_cp_rem + i)*OSF];

                // Take fft to get pilot symbols in frequency domain
    		    kiss_fft( fft_cfg, (const complex_t *)fft_in, fft_out);

                // calculate channel coefficients by dividing transmit pilots (only at odd subcarriers)
                for (int i = 1; i<N_QAM; i+=2){
                    hf[i] = comp_div( fft_out[i], sync_qam_tx[i]);
                    hf[N_FFT-i] = comp_div( fft_out[N_FFT-i], sync_qam_tx[N_FFT-i]);
                }

                // Take ifft of channel coeficient to get time domain channel response
    		    kiss_fft( ifft_cfg, (const complex_t *)hf, ht);

                // Start from mid point in the channel response since there will be two maxima
                // Maximum ata loc within       [NFFT/4...........................,NFFT/2+1,.......,N_FFT/2+max_sync_error+1,...,3*NFFT/4]
                // Corresponds to err of [NFFT/4+max_sync_err+1,.........,max_sync_error,..........0................,....,-NFFT/4+max_sync_error+1]
                complex_t *ch_ptr = ht+N_FFT/4+max_sync_error;
                // Set dominant tap location in the channel to 0
                float max_coef = comp_mag_sqr(ch_ptr[0]);
                // Compare and update the dominant tap (maximum)
                for( int i=1; i<N_FFT/2; i++) {
                    if( max_coef < comp_mag_sqr(ch_ptr[i]) ){
                        max_coef = comp_mag_sqr(ch_ptr[i]);
                        sync_err = N_FFT/4-i;
                    }
                }
                sync_corrected = 1;
                start_idx += (SYNC_SYM_LEN -  sync_err*OSF);
                fprintf(stdout,"Sync correction done by %d samples, Corrected Sync Index = %d\n", sync_err*OSF, start_idx);
            } else {
                stop_idx = sync_idx;
                fprintf(stdout,"Sync correction not done, stop_idx=%d\n", stop_idx);
            }
        }
        if (sync_corrected){
            // downsample the received data
	        while( (stop_idx > (start_idx+DATA_SYM_LEN)) && (sym_count<N_SYM)){

                start_idx = (start_idx + N_CP_DATA*OSF);
		        for( int i=0; i< N_FFT; i++){
	    	    #ifdef FLIP
		    	    fft_in[i].r = rx_sig_buff[start_idx] - rx_sig_buff[start_idx + DATA_SYM_LEN];
	    	    #elif defined(DCO)
		    	    fft_in[i].r = rx_sig_buff[start_idx];
		        #endif
                    start_idx+=OSF;
		        }

        		kiss_fft( fft_cfg, (const complex_t *)fft_in, fft_out);
	        	qam_demod( bin_rx, (fft_out+1));

		        // get bin buffer pointer for next symbol
		        bin_rx += N_QAM*N_BITS;
                *bits_recvd += N_QAM*N_BITS;
                sym_count++;

    	    	// FLIP decodes two ofdm symbols together, so advance pointer once more
        	    #if defined(FLIP)
		        start_idx += DATA_SYM_LEN;
	            #endif
	        }

            if(sym_count==N_SYM){
                corr_count = -1;
                sync_done = 0;
                sync_corrected = 0;
                max_of_min = 0;
            }
            stop_idx = start_idx;
            fprintf(stdout,"Demoduation completed, sym_count = %d, stop_idx = %d\n", sym_count, stop_idx);
	        kiss_fft_free(fft_cfg);
	        kiss_fft_free(ifft_cfg);
        }
    }
    return stop_idx;
}


int main(int argc, char** argv){

	if(rp_Init()!=RP_OK)
		fprintf(stderr,"RX: Initialization failed");

    // received samples, remaining samples, received bits, current and previous ADC ptr pos
	uint32_t samp_recvd = 0, bits_recvd = 0, curr_pos, prev_pos=0;
    // receive binary buffer (one extra buffer to take care of spillage while checking end of buffer)
	uint8_t rx_bin_start[(N_FRAMES+1)*N_SYM*N_QAM*N_BITS] = {0.0};
    // end of last binary buffer (last frame)
	uint8_t *rx_bin_end = rx_bin_start + N_FRAMES*N_SYM*N_QAM*N_BITS;
    // current location of pointer in binary buffer
	uint8_t *rx_bin_ptr = rx_bin_start;
    // receive signal buffer (one extra buffer for the case when current read samples spill out
    // of the first buffer. In next read cycle, pointer is reset to start of the first buffer)
	uint32_t demod_idx = 0, recvd_idx = 0, end_idx = N_FRAMES*ADC_BUFFER_SIZE;
    // timing variables
    uint64_t start=0, end1=0, end2 = 0;

	fprintf(stdout, "RX: Entered\n");

    #ifdef OFFLINE
    FILE *fp;
	fp = fopen("./data.txt","r");
    #endif

    // generate the pilots for synchronization error err exactly same as in Transmit Program
    generate_ofdm_sync();
    // wait till transmission is started
	usleep(200000);
    // reset the ADC
    rp_AcqReset();
    // set the ADC sample rate (125e6/decimation)
    rp_AcqSetDecimation(RP_DEC_64);
    // enable continuous acquisition
	rp_AcqSetArmKeep(true);
    // set the trigger delay
    rp_AcqSetTriggerDelay(0);
    // set trigger source (instantaneous triggering)
    rp_AcqSetTriggerSrc(RP_TRIG_SRC_NOW);
    // start the acquisition
    rp_AcqStart();
    // small wait till ADC has acquired some data
    usleep(10000);

    // continue recieving untill receive signal buffer gets filled
	while( TRUE ){
        // get the cpu clock at the start
        start = GetTimeStamp();
        // get the current ADC write pointer
		rp_AcqGetWritePointer(&curr_pos);
        // calculate the samp_recvd of the data to be acquired
		samp_recvd = (curr_pos - prev_pos) % ADC_BUFFER_SIZE;

        // acquire the data into rx signal buffer from hardware ADC buffer/Stored Data File
        #ifdef OFFLINE
        for (int i =0; i<samp_recvd; i++)
            fscanf(fp,"%f", rx_sig_buff+i );
        #else
	    rp_AcqGetDataV(RP_CH_2, prev_pos, &samp_recvd, (rx_sig_buff+recvd_idx) );
        #endif
        // advance the recvd signal index pointer
        recvd_idx += samp_recvd;
        // publish the acquisition details
		fprintf(stdout,"RX: Read out samples = %d, Current pos = %d, Prev_pos = %d\n", samp_recvd, curr_pos, prev_pos);
        // calculate the acquisition time
        end1 = GetTimeStamp() - start;
        // demodulate the receive signal and save the remaining unprocessed samples
		demod_idx = ofdm_demod(rx_bin_ptr, demod_idx, recvd_idx, &bits_recvd);
        // update the ADC pointer position
		prev_pos = curr_pos;
        // advance the rx binary buffer
		rx_bin_ptr += bits_recvd;
        // check if sig buffer end is reached
		if ( recvd_idx > end_idx )
			break;
        // check if bin buffer end is reached
		if ( rx_bin_ptr > rx_bin_end)
			break;

        // calculate the data processing time
        end2 = GetTimeStamp() - end1 - start;
        fprintf(stdout,"RX: Read time = %lfms, Process time = %lfms\n", (double)end1/1000, (double)end2/1000);
	}

	FILE *fp1, *fp2;
	fp1 = fopen("./bin.txt","w+");
	fp2 = fopen("./sig.txt","w+");

    // save demodulated data
    for(int i = 0; i <N_FRAMES*N_SYM*N_QAM*N_BITS; i++){
        fprintf(fp1," %d \n", rx_bin_start[i]);
    }
    for(int i = 0; i < recvd_idx; i++){
        fprintf(fp2," %f \n", rx_sig_buff[i]);
    }
    fclose(fp1);
    fclose(fp2);
    #ifdef OFFLINE
    fclose(fp);
    #endif

    // Stop acquisition and release resources
    rp_AcqStop();
	rp_Release();
	fprintf(stdout,"RX: Acquisition Comeplete, Exiting.\n");
    return 0;
}

/*	FILE *fp2;
	fp2 = fopen("./data.txt","r");
	FILE *fp2;
	fp1 = fopen("./bin.txt","w+");


    start = clock();
    samp_remng = ofdm_demod(rx_bin_start, rx_sig_start, samp_recvd, &bits_recvd);

    end1 = clock() - start;
    fprintf(stdout,"RX: Read time = %lf, Process time = %lf\n", (double)end1/CLOCKS_PER_SEC, (double)end2/CLOCKS_PER_SEC);

    fprintf(stdout,"samp remaining = %d\n", samp_remng);
    for (int i=0; i<N_SYM*N_QAM*N_BITS; i++)
        fprintf(fp1,"%d\n", rx_bin_start[i]);

    fclose(fp2);
    fclose(fp1);
*/