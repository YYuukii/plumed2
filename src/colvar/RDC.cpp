/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2014,2015 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */

#include "Colvar.h"
#include "ActionRegister.h"

#ifdef __PLUMED_HAS_GSL
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_blas.h>
#endif

using namespace std;

namespace PLMD{

//+PLUMEDOC COLVAR RDC 
/*
Calculates the Residual Dipolar Coupling between two atoms. 

The RDC between two atomic nuclei depends on the \f$\theta\f$ angle between 
the inter-nuclear vector and the external magnetic field. In isotropic media RDCs average to zero because of the orientational 
averaging, but when the rotational symmetry is broken, either through the introduction of an alignment medium or for molecules 
with highly anisotropic paramagnetic susceptibility, RDCs become measurable.

\f[
D=D_{max}0.5(3\cos^2(\theta)-1)
\f]

where

\f[
D_{max}=-\mu_0\gamma_1\gamma_2h/(8\pi^3r^3)
\f]

that is the maximal value of the dipolar coupling for the two nuclear spins with gyromagnetic ratio \f$\gamma\f$. 
\f$\mu\f$ is the magnetic constant and h is the Planck constant. 

Common Gyromagnetic Ratios (C.G.S)
- H(1) 26.7513
- C(13) 6.7261
- N(15) -2.7116
- NH -72.5388
- CH 179.9319
- CN -18.2385
- CC 45.2404

This collective variable calculates the Residual Dipolar Coupling for a set of couple of atoms using the above definition. 
From the calculated RDCs and a set of experimental values it calculates either their correlation or the squared quality factor 

\f[
Q^2=\frac{\sum_i(D_i-D^{exp}_i)^2}{\sum_i(D^{exp}_i)^2}
\f]

RDCs report only on the fraction of molecules that is aligned, this means that comparing the RDCs from a single structure in
a MD simulation to the experimental values is not particularly meaningfull, from this point of view it is better to compare
their correlation. The fraction of aligned molecules can be obtained by maximising the correlation between the calculated and 
the experimental RDCs. This fraction can be used as a scaling factor in the calculation of the RDCs in order to compare their
values. The averaging of the RDCs calculated with the above definition from a standard MD should result to 0 because of
the rotational diffusion, but this variable can be used to break the rotational symmetry.

RDCs can also be calculated using a Single Value Decomposition approach, in this case the code rely on the
a set of function from the GNU Scientific Library (GSL). (With SVD forces are not currently implemented).

Replica-Averaged restrained simulations can be performed with this CV and the function \ref ENSEMBLE.

Additional material and examples can be also found in the tutorial \ref belfast-9

\par Examples
In the following example five N-H RDCs are defined and their correlation with
respect to a set of experimental data is calculated and restrained. In addition,
and only for analysis purposes, the same RDCs are calculated using a Single Value
Decomposition algorithm. 

\verbatim
RDC ...
GYROM=-72.5388
SCALE=1.0 
ATOMS1=20,21
ATOMS2=37,38
ATOMS3=56,57
ATOMS4=76,77
ATOMS5=92,93
LABEL=nh
... RDC

STATS ARG=nh.* PARAMETERS=8.17,-8.271,-10.489,-9.871,-9.152

rdce: RESTRAINT ARG=nh.corr KAPPA=0. SLOPE=-25000.0 AT=1.

RDC ...
GYROM=-72.5388
SCALE=1.0
SVD 
ATOMS1=20,21 COUPLING1=8.17
ATOMS2=37,38 COUPLING2=-8.271
ATOMS3=56,57 COUPLING3=-10.489
ATOMS4=76,77 COUPLING4=-9.871
ATOMS5=92,93 COUPLING5=-9.152
LABEL=svd
... RDC

PRINT ARG=nh.corr,rdce.bias FILE=colvar
PRINT ARG=svd.* FILE=svd
\endverbatim
(See also \ref PRINT, \ref RESTRAINT) 

*/
//+ENDPLUMEDOC

class RDC : public Colvar {
private:
  const double   Const;
  unsigned ndata;
  vector<double> mu_s;
  vector<double> scale;
  vector<double> coupl;
  bool serial;
  bool svd;
public:
  explicit RDC(const ActionOptions&);
  static void registerKeywords( Keywords& keys );
  virtual void calculate();
};

PLUMED_REGISTER_ACTION(RDC,"RDC")

void RDC::registerKeywords( Keywords& keys ){
  Colvar::registerKeywords( keys );
  keys.add("numbered","ATOMS","the couple of atoms involved in each of the bonds for which you wish to calculate the RDC. "
                             "Keywords like ATOMS1, ATOMS2, ATOMS3,... should be listed and one dipolar coupling will be "
                             "calculated for each ATOMS keyword you specify.");
  keys.reset_style("ATOMS","atoms");
  keys.add("numbered","GYROM","Add the product of the gyromagnetic constants for each bond. ");
  keys.add("numbered","SCALE","Add a scaling factor to take into account concentration and other effects. ");
  keys.add("numbered","COUPLING","Add an experimental value for each coupling (only for SVD).");
  keys.addFlag("SERIAL",false,"Set to TRUE if you want to run the CV in serial.");  
  keys.addFlag("SVD",false,"Set to TRUE if you want to backcalculate using Single Value Decomposition (need GSL at compilation time).");  
  keys.addOutputComponent("rdc","default","the # RDC");
}

RDC::RDC(const ActionOptions&ao):
PLUMED_COLVAR_INIT(ao),
Const(0.3356806)
{
  // Read in the atoms
  vector<AtomNumber> t, atoms;
  for(int i=1;;++i ){
     parseAtomList("ATOMS", i, t );
     if( t.empty() ) break;
     if( t.size()!=2 ){
         std::string ss; Tools::convert(i,ss);
         error("ATOMS" + ss + " keyword has the wrong number of atoms");
     }
     atoms.push_back(t[0]);
     atoms.push_back(t[1]);
     t.resize(0); 
  }

  ndata = atoms.size()/2;
 
  // Read in GYROMAGNETIC constants 
  mu_s.resize( ndata ); 
  unsigned ntarget=0;
  for(unsigned i=0;i<ndata;++i){
     if( !parseNumbered( "GYROM", i+1, mu_s[i] ) ) break;
     ntarget++; 
  }
  if( ntarget==0 ){
      parse("GYROM",mu_s[0]);
      for(unsigned i=1;i<ndata;++i) mu_s[i]=mu_s[0];
  } else if( ntarget!=ndata ) error("found wrong number of GYROM values");

  // Read in SCALING factors 
  scale.resize( ndata );
  for(unsigned i=0;i<ndata;++i) scale[i]=1.0;
  ntarget=0;
  for(unsigned i=0;i<ndata;++i){
     if( !parseNumbered( "SCALE", i+1, scale[i] ) ) break;
     ntarget++; 
  }
  if( ntarget==0 ){
      parse("SCALE",scale[0]);
      for(unsigned i=1;i<ndata;++i) scale[i]=scale[0];
  } else if( ntarget!=ndata ) error("found wrong number of SCALE values");

  svd=false;
  parseFlag("SVD",svd);
#ifndef __PLUMED_HAS_GSL
  if(svd) error("You CANNOT use SVD without GSL. Recompile PLUMED with GSL!\n");
#endif
  if(svd) {
    coupl.resize( atoms.size()/2 ); 
    ntarget=0;

    for(unsigned i=0;i<ndata;++i){
       if( !parseNumbered( "COUPLING", i+1, coupl[i] ) ) break;
       ntarget++; 
    }
    if( ntarget!=ndata ) error("found wrong number of COUPLING values");
  }

  serial=false;
  parseFlag("SERIAL",serial);

  // Ouput details of all contacts 
  for(unsigned i=0;i<ndata;++i){
    log.printf("  The %dth Bond Dipolar Coupling is calculated from atoms : %d %d.", i+1, atoms[2*i].serial(), atoms[2*i+1].serial()); 
    log.printf("  Gyromagnetic moment is %f. Scaling factor is %f.\n",mu_s[i],scale[i]);
  }

  checkRead();

  if(svd) serial=true;

  for(unsigned i=0;i<ndata;i++) {
    std::string num; Tools::convert(i,num);
    if(!svd) {
      addComponentWithDerivatives("rdc_"+num);
      componentIsNotPeriodic("rdc_"+num);
    } else {
      addComponent("rdc_"+num);
      componentIsNotPeriodic("rdc_"+num);
    }
  }

  requestAtoms(atoms);

  log.printf("  DONE!\n"); log.flush();
}

void RDC::calculate()
{
  vector<double> rdc( ndata );
  unsigned N = getNumberOfAtoms();
  vector<Vector> dRDC(N);
  vector<Tensor> dervir(ndata);

  // internal parallelisation
  unsigned stride=2*comm.Get_size();
  unsigned rank=2*comm.Get_rank();
  if(serial){
    stride=2;
    rank=0;
  }

  if(!svd) {

    /* RDC Calculations and forces */
    for(unsigned r=rank;r<N;r+=stride)
    {
      unsigned index=r/2;
      Vector distance;
      distance=pbcDistance(getPosition(r),getPosition(r+1));
      double d    = distance.modulo();
      double ind  = 1./d;
      double id3  = ind*ind*ind; 
      double id7  = id3*id3*ind;
      double id9  = id7*ind*ind;
      double max  = -Const*scale[index]*mu_s[index];
      double dmax = id3*max;
      double cos_theta = distance[2]*ind;
      rdc[index] = 0.5*dmax*(3.*cos_theta*cos_theta-1.);
      double x2=distance[0]*distance[0];
      double y2=distance[1]*distance[1];
      double z2=distance[2]*distance[2];
      double prod = -max*id7*(1.5*x2 +1.5*y2 -6.*z2);
      dRDC[r][0] = prod*distance[0];
      dRDC[r][1] = prod*distance[1];
      dRDC[r][2] = -max*id9*distance[2]*(4.5*x2*x2 + 4.5*y2*y2 + 1.5*y2*z2 - 3.*z2*z2 + x2*(9.*y2 + 1.5*z2));
      dRDC[r+1] = -dRDC[r];
      dervir[index] += Tensor(distance,dRDC[r]);
    }
    if(!serial){
      comm.Sum(&dRDC[0][0],3*N);
      comm.Sum(&dervir[0][0][0],9*ndata);
    }
    unsigned index=0;
    for(unsigned r=0;r<N;r+=2) {
      Value* val=getPntrToComponent(index);
      val->set(rdc[index]);
      setBoxDerivatives(val, dervir[index]);
      setAtomsDerivatives( val, r  , dRDC[r  ]);
      setAtomsDerivatives( val, r+1, dRDC[r+1]); 
      index++;
    }

  } else {

#ifdef __PLUMED_HAS_GSL
    gsl_vector *rdc_vec, *S, *Stmp, *work, *bc;
    gsl_matrix *coef_mat, *A, *V;
    double Sxx,Syy,Szz,Sxy,Sxz,Syz;
    rdc_vec = gsl_vector_alloc(coupl.size());
    bc = gsl_vector_alloc(coupl.size());
    Stmp = gsl_vector_alloc(5);
    S = gsl_vector_alloc(5);
    work = gsl_vector_alloc(5);
    coef_mat = gsl_matrix_alloc(coupl.size(),5);
    A = gsl_matrix_alloc(coupl.size(),5);
    V = gsl_matrix_alloc(5,5);
    gsl_matrix_set_zero(coef_mat);
    gsl_vector_set_zero(bc);
    unsigned index=0;
    vector<double> dmax(coupl.size());
    for(unsigned r=0; r<N; r+=2) {
      Vector distance;
      distance=pbcDistance(getPosition(r),getPosition(r+1));
      double d    = distance.modulo();
      double d2   = d*d;
      double d3   = d2*d;
      double id3  = 1./d3; 
      double max  = -Const*mu_s[index]*scale[index];
      dmax[index] = id3*max;
      double mu_x = distance[0]/d;
      double mu_y = distance[1]/d;
      double mu_z = distance[2]/d;
      gsl_vector_set(rdc_vec,index,coupl[index]/dmax[index]);
      gsl_matrix_set(coef_mat,index,0,gsl_matrix_get(coef_mat,index,0)+(mu_x*mu_x-mu_z*mu_z));
      gsl_matrix_set(coef_mat,index,1,gsl_matrix_get(coef_mat,index,1)+(mu_y*mu_y-mu_z*mu_z));
      gsl_matrix_set(coef_mat,index,2,gsl_matrix_get(coef_mat,index,2)+(2.0*mu_x*mu_y));
      gsl_matrix_set(coef_mat,index,3,gsl_matrix_get(coef_mat,index,3)+(2.0*mu_x*mu_z));
      gsl_matrix_set(coef_mat,index,4,gsl_matrix_get(coef_mat,index,4)+(2.0*mu_y*mu_z));
      index++;
    }
    gsl_matrix_memcpy(A,coef_mat);
    gsl_linalg_SV_decomp(A, V, Stmp, work);
    gsl_linalg_SV_solve(A, V, Stmp, rdc_vec, S);
    Sxx = gsl_vector_get(S,0);
    Syy = gsl_vector_get(S,1);
    Szz = -Sxx-Syy;
    Sxy = gsl_vector_get(S,2);
    Sxz = gsl_vector_get(S,3);
    Syz = gsl_vector_get(S,4);
    gsl_blas_dgemv(CblasNoTrans, 1.0, coef_mat, S, 0., bc);
    for(index=0; index<coupl.size(); index++) {
      rdc[index] = gsl_vector_get(bc,index)*dmax[index];
    }
    gsl_matrix_free(coef_mat);
    gsl_matrix_free(A);
    gsl_vector_free(rdc_vec);
    gsl_vector_free(bc);
    gsl_vector_free(Stmp);
    gsl_vector_free(S);
    gsl_vector_free(work);

    for(unsigned i=0;i<ndata;i++) {
      Value* val=getPntrToComponent(i);
      val->set(rdc[i]);
    }

#endif

  }

}

}
