#define TMB_LIB_INIT R_init_sdmTMB
#include <TMB.hpp>

template <class Type>
bool isNA(Type x) {
  return R_IsNA(asDouble(x));
}

template <class Type>
Type InverseLogitPlus1(Type x) {
  return invlogit(x) + 1.0;
}

template <class Type>
vector<Type> AsVector(array<Type> x) {
  x = x.transpose();
  int nr = x.rows();
  int nc = x.cols();
  vector<Type> res(nr * nc);
  for (int i = 0; i < nr; i++)
    for (int j = 0; j < nc; j++) {
      res[i * nc + j] = x(i, j);
    }
  return res;
}

template <class Type>
vector<Type> ArrayToVector(array<Type> x) {
  int n = x.size();
  vector<Type> res(n);
  for (int i = 0; i < n; i++) res[i] = x(i);
  return res;
}

template <class Type>
Type InverseLink(Type eta, int link) {
  Type out;
  switch (link) {
    case 1:  // identity
      out = eta;
      break;
    case 2:  // log
      out = exp(eta);
      break;
    case 3:  // logit
      out = eta; // don't touch it since we're using dbinom_robust() in logit space
      break;
    default:
      error("Link not implemented.");
  }
  return out;
}

template <class Type>
Type objective_function<Type>::operator()() {
  using namespace R_inla;
  using namespace density;
  using namespace Eigen;

  //// Data inputs
  DATA_FACTOR(s_i);   // Random effect index for observation i
  DATA_INTEGER(n_t);  // number of years
  DATA_INTEGER(n_s);  // number of sites (grids)

  // Indices for factors
  DATA_FACTOR(year_i);

  // Prediction?
  DATA_INTEGER(do_predict);

  DATA_INTEGER(family);
  DATA_INTEGER(link);

  // vectors of real data
  DATA_VECTOR(y_i);   // response
  DATA_MATRIX(X_ij);  // model matrix
  // DATA_MATRIX(X_ij_phi);  // model matrix for phi

  // SPDE objects from R-INLA
  DATA_STRUCT(spde,spde_aniso_t);
  PARAMETER_VECTOR(ln_H_input);

  // Projections:
  DATA_SPARSE_MATRIX(proj_mesh);
  DATA_MATRIX(proj_X_ij);
  // DATA_IVECTOR(which_predict);

  // Parameters
  // Fixed effects
  PARAMETER_VECTOR(b_j);  // fixed effect parameters
  // PARAMETER_VECTOR(b_j_phi);  // fixed effect parameters
  PARAMETER(ln_tau_E);  // spatio-temporal process
  PARAMETER(ln_kappa);  // decorrelation distance (kind of)
  // PARAMETER(ln_obs);        // Observation variance

  PARAMETER(logit_p);  // tweedie only
  PARAMETER(log_phi);  // sigma / dispersion / etc.

  // Random effects
  // This is a matrix of (centers by years)
  PARAMETER_ARRAY(epsilon_st);  // spatio-temporal effects; n_s by n_t matrix

  // Objective function is sum of negative log likelihood components
  int n_i = y_i.size();     // number of observations
  Type nll_likelihood = 0;  // likelihood of data
  Type nll_epsilon = 0;     // spatio-temporal effects

  // Derived quantities, given parameters

  // Geospatial (Matern)
  Type Range = sqrt(8.0) / exp(ln_kappa);
  Type SigmaE =
      1.0 / sqrt(4.0 * M_PI * exp(2.0 * ln_tau_E) * exp(2.0 * ln_kappa));

  // Anisotropic Q SPDE:
  matrix<Type> H(2,2);
  H(0,0) = exp(ln_H_input(0));
  H(1,0) = ln_H_input(1);
  H(0,1) = ln_H_input(1);
  H(1,1) = (1 + ln_H_input(1) * ln_H_input(1)) / exp(ln_H_input(0));
  Eigen::SparseMatrix<Type> Q = R_inla::Q_spde(spde, exp(ln_kappa), H);

  // Linear predictor
  vector<Type> linear_predictor_i = X_ij * b_j;
  // Linear predictor on dispersion:
  // vector<Type> log_phi = X_ij_phi * b_j_phi;
  vector<Type> mu_i(n_i), eta_i(n_i);
  for (int i = 0; i < n_i; i++) {
    eta_i(i) = linear_predictor_i(i) + epsilon_st(s_i(i), year_i(i));
    mu_i(i) = InverseLink(eta_i(i), link);
  }

  // Probability of random effects
  // Spatio-temporal effects:
  for (int t = 0; t < n_t; t++)
    nll_epsilon += SCALE(density::GMRF(Q), 1.0 / exp(ln_tau_E))(epsilon_st.col(t));

  // Probability of the data, given random effects
  for (int i = 0; i < n_i; i++) {
    if (!isNA(y_i(i))) {
      switch (family) {
        case 1:  // Gaussian
          nll_likelihood -=
              dnorm(y_i(i), mu_i(i), exp(log_phi), true /* log */);
          break;
        case 2:  // Tweedie
          nll_likelihood -=
              dtweedie(y_i(i), mu_i(i), exp(log_phi),
                       InverseLogitPlus1(logit_p), true /* log */);
          break;
        case 3:  // binomial
          nll_likelihood -= dbinom_robust(y_i(i), Type(1.0) /* size */, mu_i(i),
                                          true /* log */);
          break;
        default:
          error("Family not implemented.");
      }
    }
  }

  // Calculate joint negative log likelihood
  Type jnll = nll_likelihood + nll_epsilon;

  // Projections
  if (do_predict) {
    vector<Type> proj_fe = proj_X_ij * b_j;
    array<Type> proj_re_st(proj_mesh.rows(), n_t);
    for (int i = 0; i < n_t; i++) {
      proj_re_st.col(i) = proj_mesh * ArrayToVector(epsilon_st.col(i));
    }
    vector<Type> proj_re_st_vector = AsVector(proj_re_st);
    vector<Type> proj_eta = proj_fe + proj_re_st_vector;
    REPORT(proj_fe);
    REPORT(proj_re_st_vector);
    REPORT(proj_eta);
  }

  // Reporting
  REPORT(b_j)  // fixed effect parameters
  // REPORT(b_j_phi)    // fixed effect parameters
  REPORT(ln_tau_E);  // spatio-temporal process
  REPORT(ln_kappa);  // decorrelation distance (kind of)
  REPORT(log_phi);   // Observation dispersion
  // REPORT(logit_p);   // Observation Tweedie mixing parameter

  REPORT(epsilon_st);  // spatio-temporal effects; n_s by n_t matrix

  // vector<Type> proj_mu_select = proj_mu(which_predict);

  REPORT(eta_i);
  REPORT(linear_predictor_i);
  REPORT(H);
  REPORT(Range);

  // ADREPORT(proj_mu);
  // if (do_predict) ADREPORT(mu_i);

  return jnll;
}
