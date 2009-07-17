#include "classifier/svm/MKLClassification.h"
#include "kernel/CombinedKernel.h"

CMKLClassification::CMKLClassification(CSVM* s) : CMKL(s), w_gap(1.0), rho(0)
{
}

CMKLClassification::~CMKLClassification()
{
}

struct S_THREAD_PARAM 
{
	float64_t * lin ;
	float64_t* W;
	int32_t start, end;
	int32_t * active2dnum ;
	int32_t * docs ;
	CKernel* kernel ;
};


bool CMKLClassification::perform_mkl_step(
		const float64_t* sumw, float64_t suma)
{
	int32_t num_kernels = kernel->get_num_subkernels();
	int32_t nweights=0;
	const float64_t* old_beta = kernel->get_subkernel_weights(nweights);
	ASSERT(nweights==num_kernels);
	float64_t* beta = new float64_t[num_kernels];

	int32_t inner_iters=0;
	float64_t mkl_objective=0;

	mkl_objective=-suma;
	for (int32_t i=0; i<num_kernels; i++)
	{
		beta[i]=old_beta[i];
		mkl_objective+=old_beta[i]*sumw[i];
	}

	SG_PRINT("rho=%f mkl_obj=%f\n", rho, mkl_objective);

	w_gap = CMath::abs(1-rho/mkl_objective) ;
	if( (w_gap >= 0.9999*mkl_epsilon) || (get_solver_type()==ST_INTERNAL && mkl_norm>1) )
	{
		if ( mkl_norm == 1)
		{
#ifdef USE_CPLEX
			if (get_solver_type()==ST_CPLEX || get_solver_type()==ST_AUTO)
				rho=compute_optimal_betas_via_cplex(beta, old_beta, num_kernels, sumw, suma, inner_iters);
			else
				rho=compute_optimal_betas_via_glpk(beta, old_beta, num_kernels, sumw, suma, inner_iters);
#else
			if (get_solver_type()==ST_GLPK || get_solver_type()==ST_AUTO)
				rho=compute_optimal_betas_via_glpk(beta, old_beta, num_kernels, sumw, suma, inner_iters);
			else
				rho=compute_optimal_betas_via_cplex(beta, old_beta, num_kernels, sumw, suma, inner_iters);
#endif
		}
		else
		{
			if (get_solver_type()==ST_CPLEX) {
				rho=compute_optimal_betas_via_cplex(beta, old_beta, num_kernels, sumw, suma, inner_iters);
			}
			else
				rho=compute_optimal_betas_newton(beta, old_beta, num_kernels, sumw, suma, mkl_objective);
		}

		w_gap = CMath::abs(1-rho/mkl_objective) ;
	}

	kernel->set_subkernel_weights(beta, num_kernels);
	CMath::display_vector(old_beta, num_kernels, "old_beta");
	CMath::display_vector(beta, num_kernels, "beta");

	return converged();
}


float64_t CMKLClassification::compute_optimal_betas_analytically(
  float64_t* beta, const float64_t* old_beta, const int32_t num_kernels,
  const int32_t* label, const float64_t* sumw, const float64_t suma,
  const float64_t mkl_objective )
{
  const float64_t epsRegul = 0.01;  // fraction of root mean squared deviation
  float64_t obj;
  float64_t Z;
  float64_t R;
  int32_t p;

  // --- optimal beta
  for( p=0; p<num_kernels; ++p ) {
    if( sumw[p] >= 0.0 && old_beta[p] >= 0.0 ) {
      beta[p] = sumw[p] * old_beta[p]*old_beta[p] / mkl_norm;
      beta[p] = CMath::pow( beta[p], 1.0 / (mkl_norm+1.0) );
    } else {
      beta[p] = 0.0;
    }
  }

  // --- normalize
  Z = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    Z += CMath::pow( beta[p], mkl_norm );
  }
  Z = CMath::pow( Z, -1.0/mkl_norm );
  for( p=0; p<num_kernels; ++p ) {
    beta[p] *= Z;
  }

  // --- regularize & renormalize
  R = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    R += CMath::pow( old_beta[p] - beta[p], 2.0 );
  }
  R = CMath::sqrt( R / mkl_norm ) * epsRegul;
  Z = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    beta[p] += R;
    Z += CMath::pow( beta[p], mkl_norm );
  }
  Z = CMath::pow( Z, -1.0/mkl_norm );
  for( p=0; p<num_kernels; ++p ) {
    beta[p] *= Z;
    ASSERT( beta[p] >= 0.0 );
    if( beta[p] > 1.0 ) {
      beta[p] = 1.0;
    }
  }

  // --- objective
  obj = -suma;
  for( p=0; p<num_kernels; ++p ) {
    obj += sumw[p] * old_beta[p]*old_beta[p] / beta[p];
  }
  return obj;
}

/*
float64_t CMKLClassification::compute_optimal_betas_gradient(float64_t* beta,
		float64_t* old_beta, int32_t num_kernels,
		const float64_t* sumw, float64_t suma,
    float64_t mkl_objective)
{
	SG_DEBUG("MKL via GRADIENT\n");

	const float64_t r = mkl_norm / ( mkl_norm - 1.0 );
	float64_t Z;
	float64_t obj;
	float64_t gamma;
	float64_t maxstep;
  int32_t p;

	SG_PRINT( "OBJ_old = %f\n", mkl_objective );

	gamma = 0.0;
	for( p=0; p<num_kernels; ++p ) {
		gamma += CMath::pow( sumw[p], r );
	}
  gamma = CMath::pow( gamma, 1.0/r ) / mkl_norm;

  // compute gradient (stored in "beta")
  maxstep = CMath::INFTY;
  maxstep = 0.0;
	for( p=0; p<num_kernels; ++p ) {
    ASSERT( 0.0 <= old_beta[p] && old_beta[p] <= 1.0 );
		beta[p] = gamma * mkl_norm * CMath::pow( old_beta[p], mkl_norm-1 ) - sumw[p];
    const float64_t step = ( beta[p] > 0.0 ) ? old_beta[p]/beta[p] : (old_beta[p]-1.0)/beta[p];
    ASSERT( step >= 0.0 );
    if( step > maxstep ) {
      maxstep = step;
    }
  }
  ASSERT( maxstep > 0.0 );

  // make gradient step
	Z = 0.0;
	for( p=0; p<num_kernels; ++p ) {
		beta[p] = old_beta[p] - 0.5*maxstep*beta[p];
    //ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
    if( beta[p] < 0.0 ) {
      beta[p] = 0.0;
    }
		Z += CMath::pow( beta[p], mkl_norm );
	}
	Z = CMath::pow( Z, -1.0/mkl_norm );
	for( p=0; p<num_kernels; ++p ) {
		beta[p] *= Z;
  }

	// CMath::display_vector(beta, num_kernels, "beta_alex");
	SG_PRINT("Z_alex = %e\n", Z);

	CMath::display_vector( old_beta, num_kernels, "old_beta " );
	CMath::display_vector( beta,     num_kernels, "beta     " );
	//CMath::display_vector(beta, num_kernels, "beta_log");
	//SG_PRINT("Z_log=%f\n", Z);
	//for (int32_t i=0; i<num_kernels; i++)
		//beta[i]=(beta[i]+old_beta[i])/2;

	//CMath::scale_vector(1/CMath::qnorm(beta, num_kernels, mkl_norm), beta, num_kernels);

  obj = -suma;
	for( p=0; p<num_kernels; ++p ) {
		obj += beta[p] * (sumw[p]);
  }
	SG_PRINT( "OBJ = %f\n", obj );
	return obj;
}
*/

/*
float64_t CMKLClassification::compute_optimal_betas_gradient(float64_t* beta,
		float64_t* old_beta, int32_t num_kernels,
		const float64_t* sumw, float64_t suma,
    float64_t mkl_objective)
{
	SG_DEBUG("MKL via GRADIENT-EXP\n");

	const float64_t r = mkl_norm / ( mkl_norm - 1.0 );
	float64_t Z;
	float64_t obj;
	float64_t gamma;
  int32_t p;

	SG_PRINT( "OBJ_old = %f\n", mkl_objective );

	gamma = 0.0;
	for( p=0; p<num_kernels; ++p ) {
		gamma += CMath::pow( sumw[p], r );
	}
  gamma = CMath::pow( gamma, 1.0/r ) / mkl_norm;

  // compute gradient (stored in "beta")
	for( p=0; p<num_kernels; ++p ) {
    ASSERT( 0.0 <= old_beta[p] && old_beta[p] <= 1.0 );
		beta[p] = gamma * mkl_norm * CMath::pow( old_beta[p], mkl_norm-1 ) - sumw[p];
  }
	for( p=0; p<num_kernels; ++p ) {
    beta[p] *= ( mkl_norm - 1.0 ) * ( mkl_norm - 1.0 );
  }
	CMath::display_vector( beta, num_kernels, "grad" );

  // make gradient step in log-beta space
  Z = 0.0;
	for( p=0; p<num_kernels; ++p ) {
		beta[p] = CMath::log(old_beta[p]) - beta[p] / old_beta[p];
		beta[p] = CMath::exp( beta[p] );
    if( beta[p] != beta[p] || beta[p] == CMath::INFTY ) {
      beta[p] = 0.0;
    }
    ASSERT( beta[p] == beta[p] );
		Z += CMath::pow( beta[p], mkl_norm );
	}
	Z = CMath::pow( Z, -1.0/mkl_norm );
	CMath::display_vector( beta, num_kernels, "pre-beta " );
	for( p=0; p<num_kernels; ++p ) {
		beta[p] *= Z;
  }

	// CMath::display_vector(beta, num_kernels, "beta_alex");
	SG_PRINT("Z_alex = %e\n", Z);

	CMath::display_vector( old_beta, num_kernels, "old_beta " );
	CMath::display_vector( beta,     num_kernels, "beta     " );

  obj = -suma;
	for( p=0; p<num_kernels; ++p ) {
		obj += beta[p] * (sumw[p]);
  }
	SG_PRINT( "OBJ = %f\n", obj );
	return obj;
}
*/


/*
// === primal Newton ===
float64_t CMKLClassification::compute_optimal_betas_newton(
  float64_t* beta, const float64_t* old_beta, const int32_t num_kernels,
  const int32_t* label, const float64_t* sumw, const float64_t suma,
  const float64_t mkl_objective )
{
  const float64_t gamma = 10;
  //const float64_t margPlusEps = 1.00001;
  const float64_t epsBeta = 1e-32;
  const float64_t epsNewt = 0.01;  // line search alpha
  const float64_t epsStep = 1e-9;
  const int inLogSpace = 0;
  const int32_t nSvs = get_num_support_vectors();
  const int32_t num = kernel->get_num_vec_rhs();
  //const float64_t C = learn_parm->svm_c;
  const float64_t C = get_C1();
  float64_t* newtDir = new float64_t[ num_kernels ];
  float64_t* newBeta = new float64_t[ num_kernels ];
  float64_t newtStep;
  float64_t stepSize;
  float64_t obj;
  float64_t Z;
  int32_t i;
  int32_t jSv;
  int32_t l;
  int32_t p;

  // === init
  ASSERT( labels && kernel );
  // --- check beta
  Z = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    beta[p] = old_beta[p];
    if( !( beta[p] >= epsBeta ) ) {
      SG_WARNING( "old_beta[%d] = %e  (sumw[.]=%e);  set to %e.  ", p, beta[p], sumw[p], epsBeta );
      beta[p] = epsBeta;
    }
    ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
    Z += CMath::pow( beta[p], mkl_norm );
  }
  Z = CMath::pow( Z, -1.0/mkl_norm );
  if( !( fabs(Z-1.0) <= 1e-9 ) ) {
    SG_WARNING( "old_beta not normalized (diff=%e);  forcing normalization.  ", Z-1.0 );
    for( p=0; p<num_kernels; ++p ) {
      beta[p] *= Z;
      if( beta[p] > 1.0 ) {
	beta[p] = 1.0;
      }
      ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
    }
  }

  // === compute objective
  obj = 0.0;
  for( i=0; i<num; ++i ) {
    float64_t out_i;
    out_i = 0.0;
    for( p=0; p<num_kernels; ++p ) {
      out_i += beta[p] * W[ i*num_kernels + p ];
    }
    if( label[i]*out_i < 1.0 ) {
      obj += 1.0 - label[i]*out_i;
    }
  }
  obj *= C;
  ASSERT( obj >= 0.0 );
  for( i=0; i<num; ++i ) {
    obj += beta[p] * sumw[p];
  }
  ASSERT( obj > 0.0 );
  const float64_t oldObj = obj;
  const float64_t obj2 = -compute_mkl_primal_objective();
  if( !( fabs(obj-obj2) < 1e-6 ) ) {
    SG_PRINT( "WARN:  %e != %e primal\n", obj, obj2 );
  }
  // --- dual
  float64_t dualObj;
  dualObj = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    dualObj += CMath::pow( sumw[p], mkl_norm/(mkl_norm-1.0) );
  }
  dualObj = CMath::pow( dualObj, (mkl_norm-1.0)/mkl_norm );
  const float64_t dualReg = dualObj;
  dualObj = suma - dualObj;

  // === compute gradient
  for( p=0; p<num_kernels; ++p ) {
    newtDir[p] = 0.0;
  }
  for( jSv=0; jSv<nSvs; ++jSv ) {
    const int32_t j = get_support_vector( jSv );
    for( p=0; p<num_kernels; ++p ) {
      newtDir[p] += label[j] * W[ j*num_kernels + p ];
    }
  }
  for( p=0; p<num_kernels; ++p ) {
    newtDir[p] *= -C;
    newtDir[p] += sumw[p];
    //const float64_t eps_p = ( newtDir[p] > 0.0 ) ? newtDir[p] : 0.0;
    //newtDir[p] += -eps_p;
    newtDir[p] += gamma * mkl_norm * CMath::pow( beta[p], mkl_norm-1.0 );
  }
  
  // === compute Newton direction (divide by Hessian)
  const float64_t gqq1 = gamma * mkl_norm * (mkl_norm-1.0);
  newtStep = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    const float64_t t = newtDir[p];
    newtDir[p] /= gqq1 * CMath::pow( beta[p], mkl_norm-2.0 );
    newtStep += newtDir[p] * t;
    newtDir[p] *= -1;
  }
  ASSERT( newtStep >= 0.0 );
  
  // === line search
  stepSize = 1.0;
  while( stepSize >= epsStep ) {

    // --- perform Newton step
    Z = 0.0;
    while( Z == 0.0 ) {
      for( p=0; p<num_kernels; ++p ) {
        if( inLogSpace ) {
          newBeta[p] = beta[p] * CMath::exp( - stepSize * newtDir[p] );
        } else {
          newBeta[p] = beta[p] - stepSize * newtDir[p];
        }
        if( !( newBeta[p] >= epsBeta ) ) {
          newBeta[p] = epsBeta;
        }
        Z += CMath::pow( newBeta[p], mkl_norm );
      }
      ASSERT( 0.0 <= Z );
      Z = CMath::pow( Z, -1.0/mkl_norm );
      if( Z == 0.0 ) {
        stepSize /= 2.0;
      }
    }

    // --- normalize new beta (wrt p-norm)
    ASSERT( 0.0 < Z );
    ASSERT( Z < CMath::INFTY );
    for( p=0; p<num_kernels; ++p ) {
      newBeta[p] *= Z;
      if( newBeta[p] > 1.0 ) {
        //SG_WARNING( "beta[%d] = %e;  set to 1.  ", p, beta[p] );
        newBeta[p] = 1.0;
      }
      ASSERT( 0.0 <= newBeta[p] && newBeta[p] <= 1.0 );
    }

    // --- objective decreased?
    float64_t newtObj;
    {  // compute new objective
      newtObj = 0.0;
      l = 0;
      for( i=0; i<num; ++i ) {
	float64_t out_i;
	out_i = 0.0;
	for( p=0; p<num_kernels; ++p ) {
	  out_i += newBeta[p] * W[l];
	  ++l;
	}
	if( label[i]*out_i < 1.0 ) {
	  newtObj += 1.0 - label[i]*out_i;
	}
      }
      newtObj *= C;
      ASSERT( newtObj >= 0.0 );
      for( i=0; i<num; ++i ) {
	newtObj += newBeta[p] * sumw[p];
      }
      ASSERT( newtObj > 0.0 );
    }
    //SG_PRINT( "step = %.8f => obj = %e.  \n", stepSize, newtObj );
    if( newtObj < obj - epsNewt*stepSize*newtStep ) {
      SG_PRINT( "step %.5f [%e]\n", stepSize, epsNewt*stepSize*newtStep );
      for( p=0; p<num_kernels; ++p ) {
        beta[p] = newBeta[p];
      }
      obj = newtObj;
      break;
    }
    stepSize /= 2.0;
  }

  // === finish
  //SG_PRINT( "MKL primal [%e]: %e -> %e\n", mkl_objective, oldObj, obj );
  SG_PRINT( "dual: %e (%e) [%e];  primal: %e -> %e\n", dualObj, dualReg, mkl_objective, oldObj, obj );
  ASSERT( obj <= oldObj );
  delete[] newtDir;
  delete[] newBeta;
  //return obj;
  return mkl_objective + obj - oldObj;
}
*/



float64_t CMKLClassification::compute_optimal_betas_newton(float64_t* beta,
		const float64_t* old_beta, int32_t num_kernels,
		const float64_t* sumw, float64_t suma,
		 float64_t mkl_objective)
{
  SG_DEBUG("MKL via NEWTON\n");

  const double epsBeta = 1e-32;
  const double epsGamma = 1e-12;
  const double epsWsq = 1e-12;
  const double epsNewt = 0.0001;
  const double epsStep = 1e-9;
  const int nofNewtonSteps = 3;
  const double hessRidge = 1e-6;
  const int inLogSpace = 0;

  const float64_t r = mkl_norm / ( mkl_norm - 1.0 );
  float64_t* newtDir = new float64_t[ num_kernels ];
  float64_t* newtBeta = new float64_t[ num_kernels ];
  float64_t newtStep;
  float64_t stepSize;
  float64_t Z;
  float64_t obj;
  float64_t gamma;
  int32_t p;
  int i;


  // === init
  for( p=0; p<num_kernels; ++p ) {
    //SG_PRINT( "old_beta[%d] = %e;  sumw[%d]=%e.  \n", p, old_beta[p], p, sumw[p] );
  }

  // --- check beta
  Z = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    beta[p] = old_beta[p];
    if( !( beta[p] >= epsBeta ) ) {
      //SG_WARNING( "old_beta[%d] = %e  (sumw[.]=%e);  set to %e.  ", p, beta[p], sumw[p], epsBeta );
      beta[p] = epsBeta;
    }
    ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
    Z += CMath::pow( beta[p], mkl_norm );
  }
  Z = CMath::pow( Z, -1.0/mkl_norm );
  if( !( fabs(Z-1.0) <= epsGamma ) ) {
    SG_WARNING( "old_beta not normalized (diff=%e);  forcing normalization.  ", Z-1.0 );
    for( p=0; p<num_kernels; ++p ) {
      beta[p] *= Z;
      if( beta[p] > 1.0 ) {
	beta[p] = 1.0;
      }
      ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
    }
  }

  // --- compute gamma
  gamma = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    if( !( sumw[p] >= 0 ) ) {
      if( !( sumw[p] >= -epsWsq ) ) {
	SG_WARNING( "sumw[%d] = %e;  treated as 0.  ", p, sumw[p] );
      }
      // should better recompute sumw[] !!!
    } else {
      ASSERT( sumw[p] >= 0 );
      //gamma += CMath::pow( sumw[p] * beta[p]*beta[p], r );
      gamma += CMath::pow( sumw[p] * beta[p]*beta[p] / mkl_norm, r );
    }
  }
  gamma = CMath::pow( gamma, 1.0/r ) / mkl_norm;
  ASSERT( gamma > -1e-9 );
  if( !( gamma > epsGamma ) ) {
    SG_WARNING( "bad gamma: %e;  set to %e.  ", gamma, epsGamma );
    // should better recompute sumw[] !!!
    gamma = epsGamma;
  }
  ASSERT( gamma >= epsGamma );
  //gamma = -gamma;

  // --- compute objective
  obj = 0.0;
  for( p=0; p<num_kernels; ++p ) {
    obj += beta[p] * sumw[p];
    //obj += gamma/mkl_norm * CMath::pow( beta[p], mkl_norm );
  }
  if( !( obj >= 0.0 ) ) {
    SG_WARNING( "negative objective: %e.  ", obj );
  }
  //SG_PRINT( "OBJ = %e.  \n", obj );


  // === perform Newton steps
  if( nofNewtonSteps > 1 ) {
    //SG_DEBUG( "performing %d Newton steps.\n", nofNewtonSteps );
  }
  for( i = 0; i < nofNewtonSteps; ++i ) {

    // --- compute Newton direction (Hessian is diagonal)
    const float64_t gqq1 = mkl_norm * (mkl_norm-1.0) * gamma;
    newtStep = 0.0;
    for( p=0; p<num_kernels; ++p ) {
      ASSERT( 0.0 <= beta[p] && beta[p] <= 1.0 );
      //const float halfw2p = ( sumw[p] >= 0.0 ) ? sumw[p] : 0.0;
      //const float64_t t1 = halfw2p*beta[p] - mkl_norm*gamma*CMath::pow(beta[p],mkl_norm);
      //const float64_t t2 = 2.0*halfw2p + gqq1*CMath::pow(beta[p],mkl_norm-1.0);
      const float halfw2p = ( sumw[p] >= 0.0 ) ? (sumw[p]*old_beta[p]*old_beta[p]) : 0.0;
      const float64_t t0 = halfw2p*beta[p] - mkl_norm*gamma*CMath::pow(beta[p],mkl_norm+2.0);
      const float64_t t1 = ( t0 < 0 ) ? 0.0 : t0;
      const float64_t t2 = 2.0*halfw2p + gqq1*CMath::pow(beta[p],mkl_norm+1.0);
      if( inLogSpace ) {
	newtDir[p] = t1 / ( t1 + t2*beta[p] + hessRidge );
      } else {
	newtDir[p] = ( t1 == 0.0 ) ? 0.0 : ( t1 / t2 );
      }
      // newtStep += newtDir[p] * grad[p];
      ASSERT( newtDir[p] == newtDir[p] );
      //SG_PRINT( "newtDir[%d] = %6.3f = %e / %e \n", p, newtDir[p], t1, t2 );
    }
    //CMath::display_vector( newtDir, num_kernels, "newton direction  " );
    //SG_PRINT( "Newton step size = %e\n", Z );

    // --- line search
    stepSize = 1.0;
    while( stepSize >= epsStep ) {

      // --- perform Newton step
      Z = 0.0;
      while( Z == 0.0 ) {
	for( p=0; p<num_kernels; ++p ) {
	  if( inLogSpace ) {
	    newtBeta[p] = beta[p] * CMath::exp( + stepSize * newtDir[p] );
	  } else {
	    newtBeta[p] = beta[p] + stepSize * newtDir[p];
	  }
	  if( !( newtBeta[p] >= epsBeta ) ) {
	    newtBeta[p] = epsBeta;
	  }
	  Z += CMath::pow( newtBeta[p], mkl_norm );
	}
	ASSERT( 0.0 <= Z );
	Z = CMath::pow( Z, -1.0/mkl_norm );
	if( Z == 0.0 ) {
	  stepSize /= 2.0;
	}
      }

      // --- normalize new beta (wrt p-norm)
      ASSERT( 0.0 < Z );
      ASSERT( Z < CMath::INFTY );
      for( p=0; p<num_kernels; ++p ) {
	newtBeta[p] *= Z;
	if( newtBeta[p] > 1.0 ) {
	  //SG_WARNING( "beta[%d] = %e;  set to 1.  ", p, beta[p] );
	  newtBeta[p] = 1.0;
	}
	ASSERT( 0.0 <= newtBeta[p] && newtBeta[p] <= 1.0 );
      }

      // --- objective increased?
      float64_t newtObj;
      newtObj = 0.0;
      for( p=0; p<num_kernels; ++p ) {
	newtObj += sumw[p] * old_beta[p]*old_beta[p] / newtBeta[p];
      }
      //SG_PRINT( "step = %.8f:  obj = %e -> %e.  \n", stepSize, obj, newtObj );
      if( newtObj < obj - epsNewt*stepSize*obj ) {
	for( p=0; p<num_kernels; ++p ) {
	  beta[p] = newtBeta[p];
	}
	obj = newtObj;
	break;
      }
      stepSize /= 2.0;

    }

    if( stepSize < epsStep ) {
      break;
    }

  }
  delete[] newtDir;
  delete[] newtBeta;
  //CMath::scale_vector( 1.0/CMath::qnorm(beta,num_kernels,mkl_norm), beta, num_kernels );


  // === return new objective
  obj = -suma;
  for( p=0; p<num_kernels; ++p ) {
    //obj += beta[p] * sumw[p];
    obj += sumw[p] * old_beta[p]*old_beta[p] / beta[p];
  }
  return obj;
}



float64_t CMKLClassification::compute_optimal_betas_via_cplex(float64_t* x, const float64_t* old_beta, int32_t num_kernels,
		  const float64_t* sumw, float64_t suma, int32_t& inner_iters)
{
	SG_DEBUG("MKL via CPLEX\n");

#ifdef USE_CPLEX
	if (!lp_initialized)
	{
		SG_INFO( "creating LP\n") ;

		int32_t NUMCOLS = 2*num_kernels + 1 ;
		double   obj[NUMCOLS]; /* calling external lib */
		double   lb[NUMCOLS]; /* calling external lib */
		double   ub[NUMCOLS]; /* calling external lib */

		for (int32_t i=0; i<2*num_kernels; i++)
		{
			obj[i]=0 ;
			lb[i]=0 ;
			ub[i]=1 ;
		}

		for (int32_t i=num_kernels; i<2*num_kernels; i++)
			obj[i]= C_mkl;

		obj[2*num_kernels]=1 ;
		lb[2*num_kernels]=-CPX_INFBOUND ;
		ub[2*num_kernels]=CPX_INFBOUND ;

		int status = CPXnewcols (env, lp_cplex, NUMCOLS, obj, lb, ub, NULL, NULL);
		if ( status ) {
			char  errmsg[1024];
			CPXgeterrorstring (env, status, errmsg);
			SG_ERROR( "%s", errmsg);
		}

		// add constraint sum(w)=1;
		SG_INFO( "adding the first row\n");
		int initial_rmatbeg[1]; /* calling external lib */
		int initial_rmatind[num_kernels+1]; /* calling external lib */
		double initial_rmatval[num_kernels+1]; /* calling ext lib */
		double initial_rhs[1]; /* calling external lib */
		char initial_sense[1];

		// 1-norm MKL
		if (mkl_norm==1)
		{
			initial_rmatbeg[0] = 0;
			initial_rhs[0]=1 ;     // rhs=1 ;
			initial_sense[0]='E' ; // equality

			for (int32_t i=0; i<num_kernels; i++)
			{
				initial_rmatind[i]=i ;
				initial_rmatval[i]=1 ;
			}
			initial_rmatind[num_kernels]=2*num_kernels ;
			initial_rmatval[num_kernels]=0 ;

			status = CPXaddrows (env, lp_cplex, 0, 1, num_kernels+1, 
					initial_rhs, initial_sense, initial_rmatbeg,
					initial_rmatind, initial_rmatval, NULL, NULL);

		}
		else // 2 and q-norm MKL
		{
			initial_rmatbeg[0] = 0;
			initial_rhs[0]=0 ;     // rhs=1 ;
			initial_sense[0]='L' ; // <=  (inequality)

			initial_rmatind[0]=2*num_kernels ;
			initial_rmatval[0]=0 ;

			status = CPXaddrows (env, lp_cplex, 0, 1, 1, 
					initial_rhs, initial_sense, initial_rmatbeg,
					initial_rmatind, initial_rmatval, NULL, NULL);


			if (mkl_norm==2)
			{
				for (int32_t i=0; i<num_kernels; i++)
				{
					initial_rmatind[i]=i ;
					initial_rmatval[i]=1 ;
				}
				initial_rmatind[num_kernels]=2*num_kernels ;
				initial_rmatval[num_kernels]=0 ;

				status = CPXaddqconstr (env, lp_cplex, 0, num_kernels+1, 1.0, 'L', NULL, NULL,
						initial_rmatind, initial_rmatind, initial_rmatval, NULL);
			}
		}


		if ( status )
			SG_ERROR( "Failed to add the first row.\n");

		lp_initialized = true ;

		if (C_mkl!=0.0)
		{
			for (int32_t q=0; q<num_kernels-1; q++)
			{
				// add constraint w[i]-w[i+1]<s[i];
				// add constraint w[i+1]-w[i]<s[i];
				int rmatbeg[1]; /* calling external lib */
				int rmatind[3]; /* calling external lib */
				double rmatval[3]; /* calling external lib */
				double rhs[1]; /* calling external lib */
				char sense[1];

				rmatbeg[0] = 0;
				rhs[0]=0 ;     // rhs=0 ;
				sense[0]='L' ; // <=
				rmatind[0]=q ;
				rmatval[0]=1 ;
				rmatind[1]=q+1 ;
				rmatval[1]=-1 ;
				rmatind[2]=num_kernels+q ;
				rmatval[2]=-1 ;
				status = CPXaddrows (env, lp_cplex, 0, 1, 3, 
						rhs, sense, rmatbeg,
						rmatind, rmatval, NULL, NULL);
				if ( status )
					SG_ERROR( "Failed to add a smothness row (1).\n");

				rmatbeg[0] = 0;
				rhs[0]=0 ;     // rhs=0 ;
				sense[0]='L' ; // <=
				rmatind[0]=q ;
				rmatval[0]=-1 ;
				rmatind[1]=q+1 ;
				rmatval[1]=1 ;
				rmatind[2]=num_kernels+q ;
				rmatval[2]=-1 ;
				status = CPXaddrows (env, lp_cplex, 0, 1, 3, 
						rhs, sense, rmatbeg,
						rmatind, rmatval, NULL, NULL);
				if ( status )
					SG_ERROR( "Failed to add a smothness row (2).\n");
			}
		}
	}

	{ // add the new row
		//SG_INFO( "add the new row\n") ;

		int rmatbeg[1];
		int rmatind[num_kernels+1];
		double rmatval[num_kernels+1];
		double rhs[1];
		char sense[1];

		rmatbeg[0] = 0;
		if (mkl_norm==1)
			rhs[0]=0 ;
		else
			rhs[0]=-suma ;

		sense[0]='L' ;

		for (int32_t i=0; i<num_kernels; i++)
		{
			rmatind[i]=i ;
			if (mkl_norm==1)
				rmatval[i]=-(sumw[i]-suma) ;
			else
				rmatval[i]=-sumw[i];
		}
		rmatind[num_kernels]=2*num_kernels ;
		rmatval[num_kernels]=-1 ;

		int32_t status = CPXaddrows (env, lp_cplex, 0, 1, num_kernels+1, 
				rhs, sense, rmatbeg,
				rmatind, rmatval, NULL, NULL);
		if ( status ) 
			SG_ERROR( "Failed to add the new row.\n");
	}

	inner_iters=0;
	int status;
	{ 

		if (mkl_norm==1) // optimize 1 norm MKL
			status = CPXlpopt (env, lp_cplex);
		else if (mkl_norm==2) // optimize 2-norm MKL
			status = CPXbaropt(env, lp_cplex);
		else // q-norm MKL
		{
			float64_t* beta=new float64_t[2*num_kernels+1];
			float64_t objval_old=1e-8; //some value to cause the loop to not terminate yet
			for (int32_t i=0; i<num_kernels; i++)
				beta[i]=old_beta[i];
			for (int32_t i=num_kernels; i<2*num_kernels+1; i++)
				beta[i]=0;

			while (true)
			{
				//int rows=CPXgetnumrows(env, lp_cplex);
				//int cols=CPXgetnumcols(env, lp_cplex);
				//SG_PRINT("rows:%d, cols:%d (kernel:%d)\n", rows, cols, num_kernels);
				CMath::scale_vector(1/CMath::qnorm(beta, num_kernels, mkl_norm), beta, num_kernels);

				set_qnorm_constraints(beta, num_kernels);

				status = CPXbaropt(env, lp_cplex);
				if ( status ) 
					SG_ERROR( "Failed to optimize Problem.\n");

				int solstat=0;
				double objval=0;
				status=CPXsolution(env, lp_cplex, &solstat, &objval,
						(double*) beta, NULL, NULL, NULL);

				if ( status )
				{
					CMath::display_vector(beta, num_kernels, "beta");
					SG_ERROR( "Failed to obtain solution.\n");
				}

				CMath::scale_vector(1/CMath::qnorm(beta, num_kernels, mkl_norm), beta, num_kernels);

				//SG_PRINT("[%d] %f (%f)\n", inner_iters, objval, objval_old);
				if ((1-abs(objval/objval_old) < 0.1*weight_epsilon)) // && (inner_iters>2))
					break;

				objval_old=objval;

				inner_iters++;
			}
			delete[] beta;
		}

		if ( status ) 
			SG_ERROR( "Failed to optimize Problem.\n");

		// obtain solution
		int32_t cur_numrows=(int32_t) CPXgetnumrows(env, lp_cplex);
		int32_t cur_numcols=(int32_t) CPXgetnumcols(env, lp_cplex);
		int32_t num_rows=cur_numrows;
		ASSERT(cur_numcols<=2*num_kernels+1);

		float64_t* slack=new float64_t[cur_numrows];
		float64_t* pi=NULL;
		if (use_mkl==1)
			pi=new float64_t[cur_numrows];

		if (x==NULL || slack==NULL || pi==NULL)
		{
			status = CPXERR_NO_MEMORY;
			SG_ERROR( "Could not allocate memory for solution.\n");
		}

		/* calling external lib */
		int solstat=0;
		double objval=0;

		if (mkl_norm==1)
		{
			status=CPXsolution(env, lp_cplex, &solstat, &objval,
					(double*) x, (double*) pi, (double*) slack, NULL);
		}
		else
		{
			status=CPXsolution(env, lp_cplex, &solstat, &objval,
					(double*) x, NULL, (double*) slack, NULL);
		}

		int32_t solution_ok = (!status) ;
		if ( status )
			SG_ERROR( "Failed to obtain solution.\n");

		int32_t num_active_rows=0 ;
		if (solution_ok)
		{
			/* 1 norm mkl */
			float64_t max_slack = -CMath::INFTY ;
			int32_t max_idx = -1 ;
			int32_t start_row = 1 ;
			if (C_mkl!=0.0)
				start_row+=2*(num_kernels-1);

			for (int32_t i = start_row; i < cur_numrows; i++)  // skip first
			{
				if (mkl_norm==1)
				{
					if ((pi[i]!=0))
						num_active_rows++ ;
					else
					{
						if (slack[i]>max_slack)
						{
							max_slack=slack[i] ;
							max_idx=i ;
						}
					}
				}
				else // 2-norm or general q-norm
				{
					if ((CMath::abs(slack[i])<1e-6))
						num_active_rows++ ;
					else
					{
						if (slack[i]>max_slack)
						{
							max_slack=slack[i] ;
							max_idx=i ;
						}
					}
				}
			}

			// have at most max(100,num_active_rows*2) rows, if not, remove one
			if ( (num_rows-start_row>CMath::max(100,2*num_active_rows)) && (max_idx!=-1))
			{
				//SG_INFO( "-%i(%i,%i)",max_idx,start_row,num_rows) ;
				status = CPXdelrows (env, lp_cplex, max_idx, max_idx) ;
				if ( status ) 
					SG_ERROR( "Failed to remove an old row.\n");
			}

			//CMath::display_vector(x, num_kernels, "beta");

			rho = -x[2*num_kernels] ;
			delete[] pi ;
			delete[] slack ;

		}
		else
		{
			/* then something is wrong and we rather 
			stop sooner than later */
			rho = 1 ;
		}
	}
#else
	SG_ERROR("Cplex not enabled at compile time\n");
#endif
	return rho;
}

float64_t CMKLClassification::compute_optimal_betas_via_glpk(float64_t* beta, const float64_t* old_beta,
		int num_kernels, const float64_t* sumw, float64_t suma, int32_t& inner_iters)
{
	SG_DEBUG("MKL via GLPK\n");
	float64_t obj=1.0;
#ifdef USE_GLPK
	int32_t NUMCOLS = 2*num_kernels + 1 ;
	if (!lp_initialized)
	{
		SG_INFO( "creating LP\n") ;

		//set obj function, note glpk indexing is 1-based
		lpx_add_cols(lp_glpk, NUMCOLS);
		for (int i=1; i<=2*num_kernels; i++)
		{
			lpx_set_obj_coef(lp_glpk, i, 0);
			lpx_set_col_bnds(lp_glpk, i, LPX_DB, 0, 1);
		}
		for (int i=num_kernels+1; i<=2*num_kernels; i++)
		{
			lpx_set_obj_coef(lp_glpk, i, C_mkl);
		}
		lpx_set_obj_coef(lp_glpk, NUMCOLS, 1);
		lpx_set_col_bnds(lp_glpk, NUMCOLS, LPX_FR, 0,0); //unbound

		//add first row. sum[w]=1
		int row_index = lpx_add_rows(lp_glpk, 1);
		int ind[num_kernels+2];
		float64_t val[num_kernels+2];
		for (int i=1; i<=num_kernels; i++)
		{
			ind[i] = i;
			val[i] = 1;
		}
		ind[num_kernels+1] = NUMCOLS;
		val[num_kernels+1] = 0;
		lpx_set_mat_row(lp_glpk, row_index, num_kernels, ind, val);
		lpx_set_row_bnds(lp_glpk, row_index, LPX_FX, 1, 1);

		lp_initialized = true;

		if (C_mkl!=0.0)
		{
			for (int32_t q=1; q<num_kernels; q++)
			{
				int mat_ind[4];
				float64_t mat_val[4];
				int mat_row_index = lpx_add_rows(lp_glpk, 2);
				mat_ind[1] = q;
				mat_val[1] = 1;
				mat_ind[2] = q+1; 
				mat_val[2] = -1;
				mat_ind[3] = num_kernels+q;
				mat_val[3] = -1;
				lpx_set_mat_row(lp_glpk, mat_row_index, 3, mat_ind, mat_val);
				lpx_set_row_bnds(lp_glpk, mat_row_index, LPX_UP, 0, 0);
				mat_val[1] = -1; 
				mat_val[2] = 1;
				lpx_set_mat_row(lp_glpk, mat_row_index+1, 3, mat_ind, mat_val);
				lpx_set_row_bnds(lp_glpk, mat_row_index+1, LPX_UP, 0, 0);
			}
		}
	}

	int ind[num_kernels+2];
	float64_t val[num_kernels+2];
	int row_index = lpx_add_rows(lp_glpk, 1);
	for (int32_t i=1; i<=num_kernels; i++)
	{
		ind[i] = i;
		val[i] = -(sumw[i-1]-suma);
	}
	ind[num_kernels+1] = 2*num_kernels+1;
	val[num_kernels+1] = -1;
	lpx_set_mat_row(lp_glpk, row_index, num_kernels+1, ind, val);
	lpx_set_row_bnds(lp_glpk, row_index, LPX_UP, 0, 0);

	//optimize
	lpx_simplex(lp_glpk);
	bool res = check_lpx_status(lp_glpk);
	if (!res)
		SG_ERROR( "Failed to optimize Problem.\n");

	int32_t cur_numrows = lpx_get_num_rows(lp_glpk);
	int32_t cur_numcols = lpx_get_num_cols(lp_glpk);
	int32_t num_rows=cur_numrows;
	ASSERT(cur_numcols<=2*num_kernels+1);

	float64_t* col_primal = new float64_t[cur_numcols];
	float64_t* row_primal = new float64_t[cur_numrows];
	float64_t* row_dual = new float64_t[cur_numrows];

	for (int i=0; i<cur_numrows; i++)
	{
		row_primal[i] = lpx_get_row_prim(lp_glpk, i+1);
		row_dual[i] = lpx_get_row_dual(lp_glpk, i+1);
	}
	for (int i=0; i<cur_numcols; i++)
		col_primal[i] = lpx_get_col_prim(lp_glpk, i+1);

	obj = -col_primal[2*num_kernels];

	for (int i=0; i<num_kernels; i++)
		beta[i] = col_primal[i];

	int32_t num_active_rows=0;
	if(res)
	{
		float64_t max_slack = CMath::INFTY;
		int32_t max_idx = -1;
		int32_t start_row = 1;
		if (C_mkl!=0.0)
			start_row += 2*(num_kernels-1);

		for (int32_t i= start_row; i<cur_numrows; i++)
		{
			if (row_dual[i]!=0)
				num_active_rows++;
			else
			{
				if (row_primal[i]<max_slack)
				{
					max_slack = row_primal[i];
					max_idx = i;
				}
			}
		}

		if ((num_rows-start_row>CMath::max(100, 2*num_active_rows)) && max_idx!=-1)
		{
			int del_rows[2];
			del_rows[1] = max_idx+1;
			lpx_del_rows(lp_glpk, 1, del_rows);
		}
	}

	delete[] row_dual;
	delete[] row_primal;
	delete[] col_primal;
#else
	SG_ERROR("Glpk not enabled at compile time\n");
#endif

	return obj;
}

#ifdef USE_CPLEX
void CMKLClassification::set_qnorm_constraints(float64_t* beta, int32_t num_kernels)
{
	ASSERT(num_kernels>0);

	float64_t* grad_beta=new float64_t[num_kernels];
	float64_t* hess_beta=new float64_t[num_kernels+1];
	float64_t* lin_term=new float64_t[num_kernels+1];
	int* ind=new int[num_kernels+1];

	//CMath::display_vector(beta, num_kernels, "beta");
	double const_term = 1-CMath::qsq(beta, num_kernels, mkl_norm);
	//SG_PRINT("const=%f\n", const_term);
	ASSERT(CMath::fequal(const_term, 0.0));

	for (int32_t i=0; i<num_kernels; i++)
	{
		grad_beta[i]=mkl_norm * pow(beta[i], mkl_norm-1);
		hess_beta[i]=0.5*mkl_norm*(mkl_norm-1) * pow(beta[i], mkl_norm-2); 
		lin_term[i]=grad_beta[i] - 2*beta[i]*hess_beta[i];
		const_term+=grad_beta[i]*beta[i] - CMath::sq(beta[i])*hess_beta[i];
		ind[i]=i;
	}
	ind[num_kernels]=2*num_kernels;
	hess_beta[num_kernels]=0;
	lin_term[num_kernels]=0;

	int status=0;
	int num=CPXgetnumqconstrs (env, lp_cplex);

	if (num>0)
	{
		status = CPXdelqconstrs (env, lp_cplex, 0, 0);
		ASSERT(!status);
	}

	status = CPXaddqconstr (env, lp_cplex, num_kernels+1, num_kernels+1, const_term, 'L', ind, lin_term,
			ind, ind, hess_beta, NULL);
	ASSERT(!status);

	//CPXwriteprob (env, lp_cplex, "prob.lp", NULL);
	//CPXqpwrite (env, lp_cplex, "prob.qp");

	delete[] grad_beta;
	delete[] hess_beta;
	delete[] lin_term;
	delete[] ind;
}
#endif // USE_CPLEX

// assumes that all constraints are satisfied
float64_t CMKLClassification::compute_mkl_dual_objective()
{
	int32_t n=get_num_support_vectors();
	float64_t mkl_obj=0;

	if (labels && kernel && kernel->get_kernel_type() == K_COMBINED)
	{
		CKernel* kn = ((CCombinedKernel*)kernel)->get_first_kernel();
		while (kn)
		{
			float64_t sum=0;
			for (int32_t i=0; i<n; i++)
			{
				int32_t ii=get_support_vector(i);

				for (int32_t j=0; j<n; j++)
				{
					int32_t jj=get_support_vector(j);
					sum+=get_alpha(i)*get_alpha(j)*kn->kernel(ii,jj);
				}
			}

			if (mkl_norm==1.0)
				mkl_obj = CMath::max(mkl_obj, sum);
			else
				mkl_obj += CMath::pow(sum, mkl_norm/(mkl_norm-1));

			kn = ((CCombinedKernel*) kernel)->get_next_kernel();
		}

		if (mkl_norm==1.0)
			mkl_obj=-0.5*mkl_obj;
		else
			mkl_obj= -0.5*CMath::pow(mkl_obj, (mkl_norm-1)/mkl_norm);

		for (int32_t i=0; i<n; i++)
		{
			int32_t ii=get_support_vector(i);
			mkl_obj+=get_alpha(i)*labels->get_label(ii);
		}
	}
	else
		SG_ERROR( "cannot compute objective, labels or kernel not set\n");

	return -mkl_obj;
}
