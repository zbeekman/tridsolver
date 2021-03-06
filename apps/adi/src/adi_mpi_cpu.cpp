/*
 * Open source copyright declaration based on BSD open source template:
 * http://www.opensource.org/licenses/bsd-license.php
 *
 * This file is part of the scalar-tridiagonal solver distribution.
 *
 * Copyright (c) 2015, Endre László and others. Please see the AUTHORS file in
 * the main source directory for a full list of copyright holders.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * The name of Endre László may not be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Endre László ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Endre László BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Written by Endre Laszlo, University of Oxford, endre.laszlo@oerc.ox.ac.uk, 2013-2014 
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <float.h>

#include "adi_cpu.h"
#include "adi_mpi.h"
#include "preproc_mpi.hpp"
#include "cpu/trid_mpi_cpu.hpp"

#include "trid_common.h"

#include "omp.h"
//#include "offload.h"
#include "mpi.h"

#ifdef __MKL__
  //#include "lapacke.h"
  #include "mkl_lapacke.h"
  //#include "mkl.h"
#endif

#define ROUND_DOWN(N,step) (((N)/(step))*step) 
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))

extern char *optarg;
extern int  optind, opterr, optopt; 
static struct option options[] = {
  {"nx",   required_argument, 0,  0   },
  {"ny",   required_argument, 0,  0   },
  {"nz",   required_argument, 0,  0   },
  {"iter", required_argument, 0,  0   },
  {"opt",  required_argument, 0,  0   },
  {"prof", required_argument, 0,  0   },
  {"help", no_argument,       0,  'h' },
  {0,      0,                 0,  0   }
};

/*
 * Print essential infromation on the use of the program
 */
void print_help() {
  printf("Please specify the ADI configuration, e.g.: \n$ ./adi_* -nx NX -ny NY -nz NZ -iter ITER [-opt CUDAOPT] -prof PROF\n");
  exit(0);
}

//inline double elapsed_time(double *et) {
//  struct timeval t;
//  double old_time = *et;
//
//  gettimeofday( &t, (struct timezone *)0 );
//  *et = t.tv_sec + t.tv_usec*1.0e-6;
//
//  return *et - old_time;
//}

//inline void timing_start(int prof, double *timer, mpi_handle &mpi) {
////  if(mpi.rank==0) {
//    if(prof==1) elapsed_time(timer);
////  }
//}
//
//inline void timing_end(int prof, double *timer, double *elapsed_accumulate, char *str, mpi_handle &mpi) {
////  if(mpi.rank==0) {
//    double elapsed;
//    if(prof==1) {
//      elapsed = elapsed_time(timer);
//      *elapsed_accumulate += elapsed;
//      printf("\n elapsed %s (sec): %1.10f (s) \n", str,elapsed);
//    }
////  }
//}


int init(app_handle &app, mpi_handle &mpi, int argc, char* argv[]) {
  if( MPI_Init(&argc,&argv) != MPI_SUCCESS) { printf("MPI Couldn't initialize. Exiting"); exit(-1);}
  //MPI_Init(NULL,NULL);
  MPI_Comm_size(MPI_COMM_WORLD, &mpi.procs);
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi.rank);
  printf("MPI rank = %d \n", mpi.rank);
  mpi.stat = (MPI_Status*)  malloc(mpi.procs*sizeof(MPI_Status));
  mpi.req  = (MPI_Request*) malloc(mpi.procs*sizeof(MPI_Request));

  //int nx, ny, nz, iter, opt, prof;
  app.nx_g = 256;
  app.ny_g = 256;
  app.nz_g = 256;
  app.iter = 10;
  app.opt  = 0;
  app.prof = 1;

  app.lambda = 1.0f;

  // Process arguments
  int opt_index = 0;
  while( getopt_long_only(argc, argv, "", options, &opt_index) != -1) {
    if(strcmp((char*)options[opt_index].name,"nx"  ) == 0) app.nx_g = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"ny"  ) == 0) app.ny_g = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"nz"  ) == 0) app.nz_g = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"iter") == 0) app.iter = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"opt" ) == 0) app.opt  = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"prof") == 0) app.prof = atoi(optarg); 
    if(strcmp((char*)options[opt_index].name,"help") == 0) print_help();
  }
  
  if(mpi.rank==0) printf("\nGlobal grid dimensions: %d x %d x %d\n", app.nx_g, app.ny_g, app.nz_g);

  int nx_tmp = 1+(app.nx_g - 1) / mpi.procs;
  app.nx_pad = (1+((nx_tmp-1)/SIMD_VEC))*SIMD_VEC; // Compute local size with padding for vecotrization
  app.x_start_g = mpi.rank * nx_tmp; 
  app.x_end_g   = MIN( ((mpi.rank+1) * nx_tmp)-1, app.nx_g-1);
  app.nx = app.x_end_g - app.x_start_g + 1;
  app.ny = app.ny_g;
  app.nz = app.nz_g;

  if( app.nx>N_MAX || app.ny>N_MAX || app.nz>N_MAX ) {
    printf("Local dimension can not exceed N_MAX=%d due to hard-coded local array sizes\n", N_MAX);
    return -1;
  }

  printf("Check parameters: SIMD_WIDTH = %d, sizeof(FP) = %d, nx_pad (padded) = %d, nx = %d, x_start_g = %d, x_end_g = %d \n", SIMD_WIDTH, sizeof(FP), app.nx_pad, app.nx, app.x_start_g, app.x_end_g);

  // allocate memory for arrays
  app.h_u = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.tmp = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.du  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.ax  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.bx  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.cx  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.ay  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.by  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.cy  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.az  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.bz  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.cz  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.aa  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.cc  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  app.dd  = (FP *)_mm_malloc(sizeof(FP) * app.nx_pad * app.ny * app.nz, SIMD_WIDTH);
  // Initialize
  for(int k=0; k<app.nz_g; k++) {
    for(int j=0; j<app.ny_g; j++) {
      for(int i=0; i<app.nx; i++) {
        int ind = k*app.nx_pad*app.ny_g + j*app.nx_pad + i;
        if( (app.x_start_g==0 && i==0) || (app.x_end_g==app.nx_g-1 && i==app.nx-1) || j==0 || j==app.ny_g-1 || k==0 || k==app.nz_g-1) {
          app.h_u[ind] = 1.0f;
        } else {
          app.h_u[ind] = 0.0f;
        }
      }
    }
  }

  app.sys_len_l   = mpi.procs*2; // Reduced system size in X dim
  app.n_sys_g     = app.ny_g*app.nz_g; // ny*nz
  int n_sys_l_tmp = app.n_sys_g/mpi.procs;
  app.n_sys_l     = (1+(n_sys_l_tmp-1)/mpi.procs)*mpi.procs; 

  // Containers used to communicate reduced system
  mpi.halo_sndbuf  = (FP*) _mm_malloc(app.n_sys_l * app.sys_len_l * 3 * sizeof(FP), SIMD_WIDTH); // Send Buffer
  mpi.halo_rcvbuf  = (FP*) _mm_malloc(app.n_sys_l * app.sys_len_l * 3 * sizeof(FP), SIMD_WIDTH); // Receive Buffer
  mpi.halo_sndbuf2 = (FP*) _mm_malloc(2*app.ny*app.nz*sizeof(FP),SIMD_WIDTH); // Send Buffer
  mpi.halo_rcvbuf2 = (FP*) _mm_malloc(2*app.ny*app.nz*sizeof(FP),SIMD_WIDTH); // Receive Buffer

  // Containers used to communicate preprocess halo
  mpi.halo_sndbuf2 = (FP*) _mm_malloc(2 * app.ny_g * app.nz_g * sizeof(FP), SIMD_WIDTH); // Send Buffer
  mpi.halo_rcvbuf2 = (FP*) _mm_malloc(2 * app.ny_g * app.nz_g * sizeof(FP), SIMD_WIDTH); // Receive Buffer

  // allocate memory for arrays
  app.aa_r = (FP *) _mm_malloc(sizeof(FP) * app.sys_len_l * app.n_sys_l, SIMD_WIDTH);
  app.cc_r = (FP *) _mm_malloc(sizeof(FP) * app.sys_len_l * app.n_sys_l, SIMD_WIDTH);
  app.dd_r = (FP *) _mm_malloc(sizeof(FP) * app.sys_len_l * app.n_sys_l, SIMD_WIDTH);
  return 0;
}


void finalize(app_handle &app, mpi_handle &mpi) {
  free(mpi.stat);
  free(mpi.req);
  _mm_free(app.h_u);
  _mm_free(app.du);
  _mm_free(app.ax);
  _mm_free(app.bx);
  _mm_free(app.cx);
  _mm_free(app.ay);
  _mm_free(app.by);
  _mm_free(app.cy);
  _mm_free(app.az);
  _mm_free(app.bz);
  _mm_free(app.cz);
  _mm_free(app.aa);
  _mm_free(app.cc);
  _mm_free(app.dd);
  _mm_free(mpi.halo_sndbuf);
  _mm_free(mpi.halo_rcvbuf);
  _mm_free(app.aa_r);
  _mm_free(app.cc_r);
  _mm_free(app.dd_r);
}

int main(int argc, char* argv[]) { 
  mpi_handle mpi;
  app_handle app;
  int ret;
  init(app, mpi, argc, argv);

  // Declare and reset elapsed time counters
  double timer           = 0.0;
  double timer2          = 0.0;
  double elapsed         = 0.0;  
  double elapsed_total   = 0.0;
  double elapsed_preproc = 0.0; 
  double elapsed_trid_x  = 0.0; 
  double elapsed_trid_y  = 0.0; 
  double elapsed_trid_z  = 0.0;

//#define TIMERS 11
//  double elapsed_time[TIMERS];
//  double   timers_min[TIMERS];
//  double   timers_max[TIMERS];
//  double   timers_avg[TIMERS];
//  char   elapsed_name[TIMERS][256] = {"forward","halo1","alltoall1","halo2","reduced","halo3","alltoall2","halo4","backward","pre_mpi","pre_comp"};
  strcpy(app.elapsed_name[ 0], "forward");
  strcpy(app.elapsed_name[ 1], "halo1");
  strcpy(app.elapsed_name[ 2], "alltoall1");
  strcpy(app.elapsed_name[ 3], "halo2");
  strcpy(app.elapsed_name[ 4], "reduced");
  strcpy(app.elapsed_name[ 5], "halo3");
  strcpy(app.elapsed_name[ 6], "alltoall2");
  strcpy(app.elapsed_name[ 7], "halo4");
  strcpy(app.elapsed_name[ 8], "backward");
  strcpy(app.elapsed_name[ 9], "pre_mpi");
  strcpy(app.elapsed_name[10], "pre_comp");
  double elapsed_forward   = 0.0;
  double elapsed_reduced   = 0.0;
  double elapsed_backward  = 0.0;
  double elapsed_alltoall1 = 0.0;
  double elapsed_alltoall2 = 0.0;
  double elapsed_halo1     = 0.0;
  double elapsed_halo2     = 0.0;
  double elapsed_halo3     = 0.0;
  double elapsed_halo4     = 0.0;

  for(int i=0; i<TIMERS; i++) {
    app.elapsed_time[i] = 0.0;
    app.timers_min[i]   = DBL_MAX;
    app.timers_avg[i]   = 0.0;
    app.timers_max[i]   = 0.0;
  }

  // Warm up computation: result stored in h_tmp which is not used later
  //preproc<FP>(lambda, h_tmp, h_du, h_ax, h_bx, h_cx, h_ay, h_by, h_cy, h_az, h_bz, h_cz, nx, nx_pad, ny, nz);
  
  //int i, j, k, ind, it;
    //
    // calculate r.h.s. and set tri-diagonal coefficients
    //
    MPI_Barrier(MPI_COMM_WORLD);
    timing_start(app.prof, &timer);
      preproc_mpi<FP>(app.lambda, app.h_u, app.du, app.ax, app.bx, app.cx, app.ay, app.by, app.cy, app.az, app.bz, app.cz, app, mpi);
    MPI_Barrier(MPI_COMM_WORLD);
    timing_end(app.prof, &timer, &elapsed_preproc, "preproc");
  
    //
    // perform tri-diagonal solves in x-direction
    //
    MPI_Barrier(MPI_COMM_WORLD);
    timing_start(app.prof, &timer);
  
  for(int it=0; it<app.iter; it++) {
    // Do the modified Thomas
    //MPI_Barrier(MPI_COMM_WORLD);

    timing_start(app.prof, &timer2);
    #pragma omp parallel for  
    for(int id=0; id<app.n_sys_g; id++) {
      int base = id*app.nx_pad;
      thomas_forward(&app.ax[base],&app.bx[base],&app.cx[base],&app.du[base],&app.h_u[base],&app.aa[base],&app.cc[base],&app.dd[base],app.nx,1);
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[0], app.elapsed_name[0]);
  
    // Communicate boundary values
    // Pack boundary to a single data structure
    timing_start(app.prof, &timer2);
    #pragma omp parallel for  
    for(int id=0; id<app.n_sys_g; id++) {
      // Gather coefficients of a,c,d
      mpi.halo_sndbuf[id*3*2 + 0*2     ] = app.aa[id*app.nx_pad           ];
      mpi.halo_sndbuf[id*3*2 + 0*2 + 1 ] = app.aa[id*app.nx_pad + app.nx-1];
      mpi.halo_sndbuf[id*3*2 + 1*2     ] = app.cc[id*app.nx_pad           ];
      mpi.halo_sndbuf[id*3*2 + 1*2 + 1 ] = app.cc[id*app.nx_pad + app.nx-1];
      mpi.halo_sndbuf[id*3*2 + 2*2     ] = app.dd[id*app.nx_pad           ];
      mpi.halo_sndbuf[id*3*2 + 2*2 + 1 ] = app.dd[id*app.nx_pad + app.nx-1];
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[1], app.elapsed_name[1]);
    //if(mpi.rank==0){
    //printf("sys_len_l = %d n_sys_l = %d ; n_sys_g = %d \n",app.sys_len_l, app.n_sys_l, app.n_sys_g); 
  
    timing_start(app.prof, &timer2);
    MPI_Alltoall(mpi.halo_sndbuf, app.n_sys_l*3*2, MPI_FLOAT, mpi.halo_rcvbuf, app.n_sys_l*3*2, MPI_FLOAT, MPI_COMM_WORLD);
    timing_end(app.prof, &timer2, &app.elapsed_time[2], app.elapsed_name[2]);
  
    // Unpack boundary data
    timing_start(app.prof, &timer2);
    #pragma omp parallel for collapse(2) 
    for(int p=0; p<mpi.procs; p++) {
      for(int id=0; id<app.n_sys_l; id++) {
        //printf("p = %d is = %d \n",p,id); 
        app.aa_r[id*app.sys_len_l + p*2    ] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 0*2     ];   
        app.aa_r[id*app.sys_len_l + p*2 + 1] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 0*2 + 1 ];   
        app.cc_r[id*app.sys_len_l + p*2    ] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 1*2     ];   
        app.cc_r[id*app.sys_len_l + p*2 + 1] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 1*2 + 1 ];   
        app.dd_r[id*app.sys_len_l + p*2    ] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 2*2     ];   
        app.dd_r[id*app.sys_len_l + p*2 + 1] = mpi.halo_rcvbuf[p*app.n_sys_l*3*2 + id*3*2 + 2*2 + 1 ];   
      }
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[3], app.elapsed_name[3]);
    
    timing_start(app.prof, &timer2);
    // Compute reduced system
    #pragma omp parallel for  
    for(int id=0; id<app.n_sys_l; id++) {
      //for(int r=0; r<mpi.procs; r++) {
      //  MPI_Barrier(MPI_COMM_WORLD);
      //  if(r==mpi.rank) {
      int base = id*app.sys_len_l;
      thomas_on_reduced(&app.aa_r[base], &app.cc_r[base], &app.dd_r[base], app.sys_len_l, 1);
      //pcr_on_reduced<FP>(&aa_r[ind], &cc_r[ind], &dd_r[ind], app.sys_len_l, 1);
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[4], app.elapsed_name[4]);
    
  
    // Pack boundary solution data
    timing_start(app.prof, &timer2);
    #pragma omp parallel for  
    for(int p=0; p<mpi.procs; p++) {
      for(int id=0; id<app.n_sys_l; id++) {
        mpi.halo_rcvbuf[p*app.n_sys_l*2 + id*2    ] = app.dd_r[id*app.sys_len_l + p*2    ];    
        mpi.halo_rcvbuf[p*app.n_sys_l*2 + id*2 + 1] = app.dd_r[id*app.sys_len_l + p*2 + 1];    
      }
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[5], app.elapsed_name[5]);
  
    // Send back new values
    timing_start(app.prof, &timer2);
    MPI_Alltoall(mpi.halo_rcvbuf, app.n_sys_l*2, MPI_FLOAT, mpi.halo_sndbuf, app.n_sys_l*2, MPI_FLOAT, MPI_COMM_WORLD);
    timing_end(app.prof, &timer2, &app.elapsed_time[6], app.elapsed_name[6]);
  
    // Unpack boundary solution 
    timing_start(app.prof, &timer2);
    #pragma omp parallel for  
    for(int id=0; id<app.n_sys_g; id++) {
      // Gather coefficients of a,c,d
      app.dd[id*app.nx_pad           ] = mpi.halo_sndbuf[id*2    ];  
      app.dd[id*app.nx_pad + app.nx-1] = mpi.halo_sndbuf[id*2 + 1];  
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[7], app.elapsed_name[7]);
  
  
    // Do the backward pass of modified Thomas
    timing_start(app.prof, &timer2);
    #pragma omp parallel for  
    for(int id=0; id<app.n_sys_g; id++) {
      int ind = id*app.nx_pad;
      thomas_backward(&app.aa[ind],&app.cc[ind],&app.dd[ind],&app.h_u[ind],app.nx,1);
    }
    timing_end(app.prof, &timer2, &app.elapsed_time[8], app.elapsed_name[8]);
  } 
  MPI_Barrier(MPI_COMM_WORLD);
  timing_end(app.prof, &timer, &elapsed_trid_x, "trid-x");

{
  int nx = app.nx;
  int ny = app.ny;
  int nz = app.nz;
  int ldim = app.nx_pad;
  FP *h_u = app.h_u;
  //h_u = du;
  for(int r=0; r<mpi.procs; r++) {
    MPI_Barrier(MPI_COMM_WORLD);
    if(r==mpi.rank) {
      printf("Data on rank = %d +++++++++++++++++++++++\n", mpi.rank);
      #include "print_array.c"
    }
  }
}

//  if(mpi.rank==0) {
//  printf("Time in trid-x segments[ms]: \n[total] \t%s \t%s \t%s \t%s \t%s \t%s \t%s \t%s \t[checksum]\n", elapsed_name[0], elapsed_name[1], elapsed_name[2], elapsed_name[3], elapsed_name[4], elapsed_name[5], elapsed_name[6], elapsed_name[7]);
//  }
//  printf("RANK %d %lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf\n", 
//      mpi.rank,
//      1000.0*elapsed_trid_x , 
//      1000.0*elapsed_time[0], 
//      1000.0*elapsed_time[1], 
//      1000.0*elapsed_time[2], 
//      1000.0*elapsed_time[3], 
//      1000.0*elapsed_time[4], 
//      1000.0*elapsed_time[5], 
//      1000.0*elapsed_time[6], 
//      1000.0*elapsed_time[7], 
//      1000.0*elapsed_time[8],
//      1000.0*(elapsed_time[0] + elapsed_time[1] + elapsed_time[2] + elapsed_time[3] + elapsed_time[4] + elapsed_time[5] + elapsed_time[6] + elapsed_time[7] + elapsed_time[8]));

  // Normalize timers to one iteration
  for(int i=0; i<TIMERS; i++) 
    app.elapsed_time[i] /= app.iter;

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Reduce(app.elapsed_time,app.timers_min,TIMERS,MPI_DOUBLE,MPI_MIN,0,MPI_COMM_WORLD);
  MPI_Reduce(app.elapsed_time,app.timers_max,TIMERS,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD);
  MPI_Reduce(app.elapsed_time,app.timers_avg,TIMERS,MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
  for(int i=0; i<TIMERS; i++) 
    app.timers_avg[i] /= mpi.procs;

  sleep(1);
  for(int i=0; i<mpi.procs; i++) {
    MPI_Barrier(MPI_COMM_WORLD);
    sleep(0.2);
    if(i==mpi.rank) {
      if(mpi.rank==0) {
        printf("Time in trid-x segments[ms]: \n[total] \t[%s] \t[%s] \t[%s] \t[%s] \t[%s] \t[%s] \t[%s] \t[%s] \t[%s] \t[checksum]\n", 
            app.elapsed_name[0], app.elapsed_name[1], app.elapsed_name[2], app.elapsed_name[3], app.elapsed_name[4], app.elapsed_name[5], app.elapsed_name[6], app.elapsed_name[7], app.elapsed_name[8]);
      }
      printf("%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf \t%lf\n",
      1000.0*elapsed_trid_x , 
      1000.0*app.elapsed_time[0], 
      1000.0*app.elapsed_time[1], 
      1000.0*app.elapsed_time[2], 
      1000.0*app.elapsed_time[3], 
      1000.0*app.elapsed_time[4], 
      1000.0*app.elapsed_time[5], 
      1000.0*app.elapsed_time[6], 
      1000.0*app.elapsed_time[7], 
      1000.0*app.elapsed_time[8],
      1000.0*(app.elapsed_time[0] + app.elapsed_time[1] + app.elapsed_time[2] + app.elapsed_time[3] + app.elapsed_time[4] + app.elapsed_time[5] + app.elapsed_time[6] + app.elapsed_time[7] + app.elapsed_time[8]));
    }
  }
  
  if(mpi.rank==0) {
    //double *timers = (double*) malloc(TIMERS*mpi.procs*sizeof(double));
    //MPI_Gather(elapsed_time, TIMERS, MPI_DOUBLE, timers, TIMERS, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    //for(int i=1; i<TIMERS; i++) {
    //  timers_min[i]  = MIN(timers_min[i-1],timers[i]); 
    //  timers_max[i]  = MAX(timers_max[i-1],timers[i]); 
    //  timers_avg[i] += timers[i]; 
    //}
    //timers_avg
    printf("TimerID MIN \t\tAVG \t\tMAX \t\tSection \n");
    for(int i=0; i<TIMERS; i++) 
      printf("%d \t%lf \t%lf \t%lf \t%s \n",i,app.timers_min[i],app.timers_avg[i],app.timers_max[i],app.elapsed_name[i]);
    printf("Done.\n");
    // Print execution times
    if(app.prof == 0) { 
      printf("Avg(per iter) \n[total]\n");
      printf("%f\n", elapsed_total/app.iter);
    }
    else if(app.prof == 1) {
    printf("Time per element averaged on %d iterations: \n[total] \t[prepro] \t[trid_x] \t[trid_y] \t[trid_z]\n", app.iter);
    //printf("%e \t%e \t%e \t%e \t%e\n",
    //    (elapsed_total/app.iter  ),
    //    (elapsed_preproc/app.iter),
    //    (elapsed_trid_x/app.iter ),
    //    (elapsed_trid_y/app.iter ),
    //    (elapsed_trid_z/app.iter ));
    printf("%e \t%e \t%e \t%e \t%e\n",
        (elapsed_total/app.iter  ) / (app.nx_g * app.ny_g * app.nz_g),
        (elapsed_preproc/app.iter) / (app.nx_g * app.ny_g * app.nz_g),
        (elapsed_trid_x/app.iter ) / (app.nx_g * app.ny_g * app.nz_g),
        (elapsed_trid_y/app.iter ) / (app.nx_g * app.ny_g * app.nz_g),
        (elapsed_trid_z/app.iter ) / (app.nx_g * app.ny_g * app.nz_g));
    }
  }
  MPI_Finalize();
  return 0;

}
