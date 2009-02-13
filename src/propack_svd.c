/*
 *   R package for Singular Spectrum Analysis
 *   Copyright (c) 2009 Anton Korobeynikov <asl@math.spbu.ru>
 *
 *   This program is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public
 *   License as published by the Free Software Foundation;
 *   either version 2 of the License, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied
 *   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *   PURPOSE.  See the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc., 675 Mass Ave, Cambridge,
 *   MA 02139, USA.
 */

#include <R.h>
#include <Rmath.h>
#include <Rdefines.h>
#include <R_ext/Utils.h>
#include <R_ext/BLAS.h>

void F77_NAME(clearstat)(void);
void F77_NAME(printstat)(void);

typedef void (*mul_fn_t) (char *transa,
                          int *m, int *n,
                          double *x, double *y,
                          double *dparm, int *iparm);

void F77_NAME(dlansvd_irl) (char *which, char *jobu, char *jobv,
                            int *m, int *n,
                            int *dim, int *p,
                            int *neig,
                            int *maxiter,
                            mul_fn_t aprod,
                            double *u, int *ldu,
                            double *sigma,
                            double *bnd,
                            double *v, int *ldv,
                            double *tolin,
                            double *work, int *lwork,
                            int *iwork, int *liwork,
                            double *doption, int *ioption,
                            int *info,
                            double *dparm, int *iparm);


void F77_SUB(printint0)(char* label, int* nc, int* d) {
  int i;
  for (i = 0; i< *nc; ++i)
    Rprintf("%c", label[i]);

  Rprintf("\t%d\n", *d);
}

void F77_SUB(printdbl0)(char* label, int* nc, double* d) {
  int i;
  for (i = 0; i< *nc; ++i)
    Rprintf("%c", label[i]);

  Rprintf("\t%e\n", *d);
}

void F77_SUB(printchar0)(char* label, int* nc) {
  int i;
  for (i = 0; i< *nc; ++i)
    Rprintf("%c", label[i]);

  Rprintf("\n");
}

/* Just ordinary dense matmul routine */
void dense_matmul(char *transa,
                  int *m, int *n,
                  double *x, double *y,
                  double *dparm, int *iparm) {
  double one = 1.0, zero = 0.0; int i1 = 1;

  /* Silence an 'unused' warning */
  iparm = iparm;

  F77_CALL(dgemv)(transa, m, n, &one, dparm, m, x, &i1, &zero, y, &i1);
}


/* Get the list element named str, or return NULL */
static SEXP getListElement(SEXP list, const char *str) {
  SEXP elmt = R_NilValue, names = GET_NAMES(list);
  int i;

  for (i = 0; i < length(list); i++)
    if (strcmp(CHAR(STRING_ELT(names, i)), str) == 0) {
      elmt = VECTOR_ELT(list, i);
      break;
    }
  return elmt;
}

#define getScalarListElement(trg, list, str, type, def)   \
  do {                                                    \
    SEXP __tmp = getListElement(list, str);               \
    trg = (__tmp != R_NilValue ? type(__tmp)[0] : (def)); \
  } while(0)

/* Main driver routine for PROPACK */
SEXP propack_svd(SEXP A, SEXP ne, SEXP opts) {
  double *rA = REAL(A);
  R_len_t m, n, kmax;
  int *dimA, neig = *INTEGER(ne);
  int p, dim, maxiter, liwrk, lwrk, i, *iwork, info, verbose;
  double *wU, *wV, *work, *sigma, *bnd, tol;
  double doption[4];
  int ioption[2];
  SEXP F, U, V, res, tmp;

  /* Get source dimensions */
  dimA = INTEGER(getAttrib(A, R_DimSymbol));
  m = dimA[0]; n = dimA[1];

  /* Compute needed options */

  /* Fix number of requested eigentriples */
  if (neig > m) neig = m;
  if (neig > n) neig = n;

  /* Maximum number of iterations */
  getScalarListElement(kmax, opts, "kmax", INTEGER, 10*neig);
  kmax = imin2(kmax, n+1);
  kmax = imin2(kmax, m+1);

  /* Dimension of Krylov subspace */
  getScalarListElement(dim, opts, "dim", INTEGER, kmax);

  /* Number of shift per restart. */
  getScalarListElement(p, opts, "p", INTEGER, dim - neig);

  /* Maximum number of restarts */
  getScalarListElement(maxiter, opts, "maxiter", INTEGER, 10);

  /* Tolerance */
  getScalarListElement(tol, opts, "tol", REAL, 1e-12);

  /* Verboseness */
  getScalarListElement(verbose, opts, "verbose", LOGICAL, 0);

  /* Level of orthogonality to maintain among Lanczos vectors. */
  doption[0] = sqrt(DOUBLE_EPS);
  /* During reorthogonalization, all vectors with with components larger than
     this value along the latest Lanczos vector c will be purged. */
  doption[1] = pow(DOUBLE_EPS, 3.0/4.0);
  /* Estimate of || A ||. */
  doption[2] = 0.0;
  /* Smallest relgap between any shift the smallest requested Ritz value. */
  doption[3] = 0.002;

  /* Reorthogonalization is done using iterated modified Gram-Schmidt. */
  ioption[0] = 0;
  /* Extended local orthogonality is enforced among u_{k}, u_{k+1} and v_{k}
     and v_{k+1} respectively. */
  ioption[1] = 1;

  /* Size of iwork */
  liwrk = 8*kmax;
  /* Size of work */
  lwrk = m+n+14*kmax+8*kmax*kmax+32*m+9;

  /* Allocate work buffers */
  work = (double*)Calloc(lwrk, double);
  iwork = (int*)Calloc(liwrk, int);
  wU = (double*)R_alloc(m, (kmax+1)*sizeof(double));
  wV = (double*)R_alloc(n, kmax*sizeof(double));
  sigma = (double*)R_alloc(kmax, sizeof(double));
  bnd = (double*)Calloc(kmax, double);

  /* Set first column of U to zero. This will make dlansvd_irl generate a random
     starting vector */
  memset(wU, 0, m*sizeof(double));

  F77_CALL(clearstat)();
  F77_CALL(dlansvd_irl)("L", "Y", "Y",
                        &m, &n,
                        &dim, &p, &neig, &maxiter,
                        dense_matmul,
                        wU, &m,
                        sigma, bnd,
                        wV, &n,
                        &tol,
                        work, &lwrk,
                        iwork, &liwrk,
                        doption, ioption,
                        &info,
                        rA, NULL);

  /* Cleanup */
  Free(work); Free(iwork); Free(bnd);

  /* Print some additional information */
  if (verbose)
    F77_CALL(printstat)();

  /* Check the return code */
  if (info > 0)
    warning("Invariant subspace of dimension %d was found.", info);
  else if (info < 0)
    error("%d singular triplets did not converge within %d iterations.",
          neig, kmax);

  /* Form the result */
  PROTECT(F = allocVector(REALSXP, neig));
  PROTECT(U = allocMatrix(REALSXP, m, neig));
  PROTECT(V = allocMatrix(REALSXP, n, neig));

  Memcpy(REAL(F), sigma, neig);
  Memcpy(REAL(U), wU, neig*m);
  Memcpy(REAL(V), wV, neig*n);

  PROTECT(res = list3(F, U, V));
  SET_TAG(res, install("d"));
  SET_TAG(CDR(res), install("u"));
  SET_TAG(CDDR(res), install("v"));

  UNPROTECT(4);
  return res;
}

