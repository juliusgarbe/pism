// Copyright (C) 2010, 2011, 2012, 2013, 2014, 2015 Constantine Khroulev and Ed Bueler
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include "ShallowStressBalance.hh"
#include "Mask.hh"
#include "PISMVars.hh"
#include "flowlaws.hh"
#include "basal_resistance.hh"
#include "pism_options.hh"
#include <cassert>

#include "error_handling.hh"

#include "SSB_diagnostics.hh"

namespace pism {

using mask::ice_free;

ShallowStressBalance::ShallowStressBalance(const IceGrid &g, EnthalpyConverter &e)
  : Component(g), basal_sliding_law(NULL), flow_law(NULL), EC(e) {

  m_vel_bc = NULL;
  bc_locations = NULL;
  sea_level = 0;

  const unsigned int WIDE_STENCIL = m_config.get("grid_max_stencil_width");

  if (m_config.get_flag("do_pseudo_plastic_till") == true) {
    basal_sliding_law = new IceBasalResistancePseudoPlasticLaw(m_config);
  } else {
    basal_sliding_law = new IceBasalResistancePlasticLaw(m_config);
  }

  m_velocity.create(m_grid, "bar", WITH_GHOSTS, WIDE_STENCIL); // components ubar, vbar
  m_velocity.set_attrs("model_state",
                       "thickness-advective ice velocity (x-component)", 
                       "m s-1", "", 0);
  m_velocity.set_attrs("model_state",
                       "thickness-advective ice velocity (y-component)",
                       "m s-1", "", 1);
  m_velocity.set_glaciological_units("m year-1");
  m_velocity.write_in_glaciological_units = true;

  basal_frictional_heating.create(m_grid, "bfrict", WITHOUT_GHOSTS);
  basal_frictional_heating.set_attrs("diagnostic",
                                     "basal frictional heating",
                                     "W m-2", "");
  basal_frictional_heating.set_glaciological_units("mW m-2");
  basal_frictional_heating.write_in_glaciological_units = true;
}

ShallowStressBalance::~ShallowStressBalance() {
  delete basal_sliding_law;
}

void ShallowStressBalance::get_diagnostics(std::map<std::string, Diagnostic*> &dict,
                                           std::map<std::string, TSDiagnostic*> &/*ts_dict*/) {
  dict["beta"]     = new SSB_beta(this);
  dict["taub"]     = new SSB_taub(this);
  dict["taub_mag"] = new SSB_taub_mag(this);
  dict["taud"]     = new SSB_taud(this);
  dict["taud_mag"] = new SSB_taud_mag(this);
}


ZeroSliding::ZeroSliding(const IceGrid &g, EnthalpyConverter &e)
  : ShallowStressBalance(g, e) {

  // Use the SIA flow law.
  IceFlowLawFactory ice_factory(m_grid.com, "sia_", m_config, &EC);
  ice_factory.setType(m_config.get_string("sia_flow_law"));

  ice_factory.setFromOptions();
  flow_law = ice_factory.create();
}

ZeroSliding::~ZeroSliding() {
  delete flow_law;
}

void ZeroSliding::add_vars_to_output(const std::string &/*keyword*/, std::set<std::string> &/*result*/)
{
  // empty
}

void ZeroSliding::define_variables(const std::set<std::string> &/*vars*/, const PIO &/*nc*/,
                                             IO_Type /*nctype*/) {
}

void ZeroSliding::write_variables(const std::set<std::string> &/*vars*/, const PIO &/*nc*/) {
}


//! \brief Update the trivial shallow stress balance object.
void ZeroSliding::update(bool fast, IceModelVec2S &melange_back_pressure) {
  (void) melange_back_pressure;

  if (not fast) {
    m_velocity.set(0.0);
    basal_frictional_heating.set(0.0);
  }
}

//! \brief Compute the basal frictional heating.
/*!
  Ice shelves have zero basal friction heating.

  \param[in] velocity *basal* sliding velocity
  \param[in] tauc basal yield stress
  \param[in] mask (used to determine if floating or grounded)
  \param[out] result
 */
void ShallowStressBalance::compute_basal_frictional_heating(const IceModelVec2V &velocity,
                                                            const IceModelVec2S &tauc,
                                                            const IceModelVec2Int &mask,
                                                            IceModelVec2S &result) {
  MaskQuery m(mask);

  IceModelVec::AccessList list;
  list.add(velocity);
  list.add(result);
  list.add(tauc);
  list.add(mask);
  
  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    if (m.ocean(i,j)) {
      result(i,j) = 0.0;
    } else {
      const double
        C = basal_sliding_law->drag(tauc(i,j), velocity(i,j).u, velocity(i,j).v),
        basal_stress_x = - C * velocity(i,j).u,
        basal_stress_y = - C * velocity(i,j).v;
      result(i,j) = - basal_stress_x * velocity(i,j).u - basal_stress_y * velocity(i,j).v;
    }
  }
}


//! \brief Compute eigenvalues of the horizontal, vertically-integrated strain rate tensor.
/*!
Calculates all components \f$D_{xx}, D_{yy}, D_{xy}=D_{yx}\f$ of the
vertically-averaged strain rate tensor \f$D\f$ [\ref SchoofStream].  Then computes
the eigenvalues `result(i,j,0)` = (maximum eigenvalue), `result(i,j,1)` = (minimum
eigenvalue).  Uses the provided thickness to make decisions (PIK) about computing
strain rates near calving front.

Note that `result(i,j,0)` >= `result(i,j,1)`, but there is no necessary relation between 
the magnitudes, and either principal strain rate could be negative or positive.

Result can be used in a calving law, for example in eigencalving (PIK).

Note: strain rates will be derived from SSA velocities, using ghosts when
necessary. Both implementations (SSAFD and SSAFEM) call
update_ghosts() to ensure that ghost values are up to date.
 */
void ShallowStressBalance::compute_2D_principal_strain_rates(const IceModelVec2V &velocity,
                                                             const IceModelVec2Int &mask,
                                                             IceModelVec2 &result) {
  double    dx = m_grid.dx(), dy = m_grid.dy();

  if (result.get_ndof() != 2) {
    throw RuntimeError("result.dof() == 2 is required");
  }

  IceModelVec::AccessList list;
  list.add(velocity);
  list.add(result);
  list.add(mask);

  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    if (ice_free(mask.as_int(i,j))) {
      result(i,j,0) = 0.0;
      result(i,j,1) = 0.0;
      continue;
    }

    planeStar<int> m = mask.int_star(i,j);
    planeStar<Vector2> U = velocity.star(i,j);

    // strain in units s-1
    double u_x = 0, u_y = 0, v_x = 0, v_y = 0,
      east = 1, west = 1, south = 1, north = 1;

    // Computes u_x using second-order centered finite differences written as
    // weighted sums of first-order one-sided finite differences.
    //
    // Given the cell layout
    // *----n----*
    // |         |
    // |         |
    // w         e
    // |         |
    // |         |
    // *----s----*
    // east == 0 if the east neighbor of the current cell is ice-free. In
    // this case we use the left- (west-) sided difference.
    //
    // If both neighbors in the east-west (x) direction are ice-free the
    // x-derivative is set to zero (see u_x, v_x initialization above).
    //
    // Similarly in other directions.
    if (ice_free(m.e)) {
      east = 0;
    }
    if (ice_free(m.w)) {
      west = 0;
    }
    if (ice_free(m.n)) {
      north = 0;
    }
    if (ice_free(m.s)) {
      south = 0;
    }

    if (west + east > 0) {
      u_x = 1.0 / (dx * (west + east)) * (west * (U.ij.u - U[West].u) + east * (U[East].u - U.ij.u));
      v_x = 1.0 / (dx * (west + east)) * (west * (U.ij.v - U[West].v) + east * (U[East].v - U.ij.v));
    }

    if (south + north > 0) {
      u_y = 1.0 / (dy * (south + north)) * (south * (U.ij.u - U[South].u) + north * (U[North].u - U.ij.u));
      v_y = 1.0 / (dy * (south + north)) * (south * (U.ij.v - U[South].v) + north * (U[North].v - U.ij.v));
    }

    const double A = 0.5 * (u_x + v_y),  // A = (1/2) trace(D)
      B   = 0.5 * (u_x - v_y),
      Dxy = 0.5 * (v_x + u_y),  // B^2 = A^2 - u_x v_y
      q   = sqrt(PetscSqr(B) + PetscSqr(Dxy));
    result(i,j,0) = A + q;
    result(i,j,1) = A - q; // q >= 0 so e1 >= e2

  }
}

//! \brief Compute 2D deviatoric stresses.
/*! Note: IceModelVec2 result has to have dof == 3. */
void ShallowStressBalance::compute_2D_stresses(const IceModelVec2V &velocity,
                                               const IceModelVec2Int &mask,
                                               IceModelVec2 &result) {
  double    dx = m_grid.dx(), dy = m_grid.dy();

  if (result.get_ndof() != 3) {
    throw RuntimeError("result.get_dof() == 3 is required");
  }

  // NB: uses constant ice hardness; choice is to use SSA's exponent; see issue #285
  double hardness = pow(m_config.get("ice_softness"),-1.0/m_config.get("ssa_Glen_exponent"));

  IceModelVec::AccessList list;
  list.add(velocity);
  list.add(result);
  list.add(mask);

  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    if (ice_free(mask.as_int(i,j))) {
      result(i,j,0) = 0.0;
      result(i,j,1) = 0.0;
      result(i,j,2) = 0.0;
      continue;
    }

    planeStar<int> m = mask.int_star(i,j);
    planeStar<Vector2> U = velocity.star(i,j);

    // strain in units s-1
    double u_x = 0, u_y = 0, v_x = 0, v_y = 0,
      east = 1, west = 1, south = 1, north = 1;

    // Computes u_x using second-order centered finite differences written as
    // weighted sums of first-order one-sided finite differences.
    //
    // Given the cell layout
    // *----n----*
    // |         |
    // |         |
    // w         e
    // |         |
    // |         |
    // *----s----*
    // east == 0 if the east neighbor of the current cell is ice-free. In
    // this case we use the left- (west-) sided difference.
    //
    // If both neighbors in the east-west (x) direction are ice-free the
    // x-derivative is set to zero (see u_x, v_x initialization above).
    //
    // Similarly in y-direction.
    if (ice_free(m.e)) {
      east = 0;
    }
    if (ice_free(m.w)) {
      west = 0;
    }
    if (ice_free(m.n)) {
      north = 0;
    }
    if (ice_free(m.s)) {
      south = 0;
    }

    if (west + east > 0) {
      u_x = 1.0 / (dx * (west + east)) * (west * (U.ij.u - U[West].u) + east * (U[East].u - U.ij.u));
      v_x = 1.0 / (dx * (west + east)) * (west * (U.ij.v - U[West].v) + east * (U[East].v - U.ij.v));
    }

    if (south + north > 0) {
      u_y = 1.0 / (dy * (south + north)) * (south * (U.ij.u - U[South].u) + north * (U[North].u - U.ij.u));
      v_y = 1.0 / (dy * (south + north)) * (south * (U.ij.v - U[South].v) + north * (U[North].v - U.ij.v));
    }

    double nu;
    flow_law->effective_viscosity(hardness,
                                  secondInvariant_2D(u_x, u_y, v_x, v_y),
                                  &nu, NULL);

    //get deviatoric stresses
    result(i,j,0) = nu*u_x;
    result(i,j,1) = nu*v_y;
    result(i,j,2) = 0.5*nu*(u_y+v_x);

  }
}

SSB_taud::SSB_taud(ShallowStressBalance *m)
  : Diag<ShallowStressBalance>(m) {

  m_dof = 2;

  // set metadata:
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taud_x", m_grid));
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taud_y", m_grid));

  set_attrs("X-component of the driving shear stress at the base of ice", "",
            "Pa", "Pa", 0);
  set_attrs("Y-component of the driving shear stress at the base of ice", "",
            "Pa", "Pa", 1);

  for (int k = 0; k < m_dof; ++k) {
    m_vars[k].set_string("comment",
                       "this field is purely diagnostic (not used by the model)");
  }
}

/*!
 * The driving stress computed here is not used by the model, so this
 * implementation intentionally does not use the eta-transformation or special
 * cases at ice margins.
 */
void SSB_taud::compute(IceModelVec* &output) {

  IceModelVec2V *result = new IceModelVec2V;
  result->create(m_grid, "result", WITHOUT_GHOSTS);
  result->metadata() = m_vars[0];
  result->metadata(1) = m_vars[1];

  const IceModelVec2S *thickness = m_grid.variables().get_2d_scalar("land_ice_thickness");
  const IceModelVec2S *surface = m_grid.variables().get_2d_scalar("surface_altitude");

  double standard_gravity = m_grid.config.get("standard_gravity"),
    ice_density = m_grid.config.get("ice_density");

  IceModelVec::AccessList list;
  list.add(*result);
  list.add(*surface);
  list.add(*thickness);

  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    double pressure = ice_density * standard_gravity * (*thickness)(i,j);
    if (pressure <= 0.0) {
      (*result)(i,j).u = 0.0;
      (*result)(i,j).v = 0.0;
    } else {
      (*result)(i,j).u = - pressure * surface->diff_x_p(i,j);
      (*result)(i,j).v = - pressure * surface->diff_y_p(i,j);
    }
  }

  output = result;
}

SSB_taud_mag::SSB_taud_mag(ShallowStressBalance *m)
  : Diag<ShallowStressBalance>(m) {

  // set metadata:
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taud_mag", m_grid));

  set_attrs("magnitude of the gravitational driving stress at the base of ice", "",
            "Pa", "Pa", 0);
  m_vars[0].set_string("comment",
                     "this field is purely diagnostic (not used by the model)");
}

void SSB_taud_mag::compute(IceModelVec* &output) {

  // Allocate memory:
  IceModelVec2S *result = new IceModelVec2S;
  result->create(m_grid, "taud_mag", WITHOUT_GHOSTS);
  result->metadata() = m_vars[0];
  result->write_in_glaciological_units = true;

  IceModelVec* tmp;
  SSB_taud diag(model);

  diag.compute(tmp);

  IceModelVec2V *taud = dynamic_cast<IceModelVec2V*>(tmp);
  if (taud == NULL) {
    delete tmp;
    throw RuntimeError("expected an IceModelVec2V, but dynamic_cast failed");
  }

  taud->magnitude(*result);

  delete tmp;

  output = result;
}

SSB_taub::SSB_taub(ShallowStressBalance *m)
  : Diag<ShallowStressBalance>(m) {
  m_dof = 2;

  // set metadata:
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taub_x", m_grid));
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taub_y", m_grid));

  set_attrs("X-component of the shear stress at the base of ice", "",
            "Pa", "Pa", 0);
  set_attrs("Y-component of the shear stress at the base of ice", "",
            "Pa", "Pa", 1);

  for (int k = 0; k < m_dof; ++k) {
    m_vars[k].set_string("comment",
                       "this field is purely diagnostic (not used by the model)");
  }
}


void SSB_taub::compute(IceModelVec* &output) {

  IceModelVec2V *result = new IceModelVec2V;
  result->create(m_grid, "result", WITHOUT_GHOSTS);
  result->metadata() = m_vars[0];
  result->metadata(1) = m_vars[1];

  IceModelVec2V *velocity;
  model->get_2D_advective_velocity(velocity);

  IceModelVec2V &vel = *velocity;
  const IceModelVec2S *tauc = m_grid.variables().get_2d_scalar("tauc");
  const IceModelVec2Int *mask = m_grid.variables().get_2d_mask("mask");

  const IceBasalResistancePlasticLaw *basal_sliding_law = model->get_sliding_law();

  MaskQuery m(*mask);

  IceModelVec::AccessList list;
  list.add(*result);
  list.add(*tauc);
  list.add(vel);
  list.add(*mask);
  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    if (m.grounded_ice(i,j)) {
      double beta = basal_sliding_law->drag((*tauc)(i,j), vel(i,j).u, vel(i,j).v);
      (*result)(i,j).u = - beta * vel(i,j).u;
      (*result)(i,j).v = - beta * vel(i,j).v;
    } else {
      (*result)(i,j).u = 0.0;
      (*result)(i,j).v = 0.0;
    }
  }

  output = result;
}

SSB_taub_mag::SSB_taub_mag(ShallowStressBalance *m)
  : Diag<ShallowStressBalance>(m) {

  // set metadata:
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "taub_mag", m_grid));

  set_attrs("magnitude of the basal shear stress at the base of ice", "",
            "Pa", "Pa", 0);
  m_vars[0].set_string("comment",
                     "this field is purely diagnostic (not used by the model)");
}

void SSB_taub_mag::compute(IceModelVec* &output) {

  // Allocate memory:
  IceModelVec2S *result = new IceModelVec2S;
  result->create(m_grid, "taub_mag", WITHOUT_GHOSTS);
  result->metadata() = m_vars[0];
  result->write_in_glaciological_units = true;

  IceModelVec* tmp;
  SSB_taub diag(model);

  diag.compute(tmp);

  IceModelVec2V *taub = dynamic_cast<IceModelVec2V*>(tmp);
  if (taub == NULL) {
    delete tmp;
    throw RuntimeError("expected an IceModelVec2V, but dynamic_cast failed");
  }

  taub->magnitude(*result);

  delete tmp;

  output = result;
}

/**
 * Shallow stress balance class that reads `u` and `v` fields from a
 * file and holds them constant.
 *
 * The only use I can think of right now is testing.
 */
PrescribedSliding::PrescribedSliding(const IceGrid &g, EnthalpyConverter &e)
  : ZeroSliding(g, e) {
  // empty
}

PrescribedSliding::~PrescribedSliding() {
  // empty
}

void PrescribedSliding::update(bool fast, IceModelVec2S &melange_back_pressure) {
  (void) melange_back_pressure;
  if (not fast) {
    basal_frictional_heating.set(0.0);
  }
}

void PrescribedSliding::init() {
  ShallowStressBalance::init();

  options::String input_filename("-prescribed_sliding_file",
                                 "name of the file to read velocity fields from");
  if (not input_filename.is_set()) {
    throw RuntimeError("option -prescribed_sliding_file is required.");
  }

  m_velocity.regrid(input_filename, CRITICAL);
}

SSB_beta::SSB_beta(ShallowStressBalance *m)
  : Diag<ShallowStressBalance>(m) {
  // set metadata:
  m_vars.push_back(NCSpatialVariable(m_grid.config.get_unit_system(), "beta", m_grid));

  set_attrs("basal drag coefficient", "", "Pa s / m", "Pa s / m", 0);
}

void SSB_beta::compute(IceModelVec* &output) {

  // Allocate memory:
  IceModelVec2S *result = new IceModelVec2S;
  result->create(m_grid, "beta", WITHOUT_GHOSTS);
  result->metadata() = m_vars[0];
  result->write_in_glaciological_units = true;

  const IceModelVec2S *tauc = m_grid.variables().get_2d_scalar("tauc");

  const IceBasalResistancePlasticLaw *basal_sliding_law = model->get_sliding_law();

  IceModelVec2V *velocity;
  model->get_2D_advective_velocity(velocity);
  IceModelVec2V &vel = *velocity;

  IceModelVec::AccessList list;
  list.add(*result);
  list.add(*tauc);
  list.add(*velocity);
  for (Points p(m_grid); p; p.next()) {
    const int i = p.i(), j = p.j();

    (*result)(i,j) =  basal_sliding_law->drag((*tauc)(i,j), vel(i,j).u, vel(i,j).v);
  }

  output = result;
}

} // end of namespace pism
