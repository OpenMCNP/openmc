#include "openmc/nuclide.h"

#include "openmc/container_util.h"
#include "openmc/endf.h"
#include "openmc/error.h"
#include "openmc/hdf5_interface.h"
#include "openmc/message_passing.h"
#include "openmc/random_lcg.h"
#include "openmc/search.h"
#include "openmc/settings.h"
#include "openmc/simulation.h"
#include "openmc/string_utils.h"
#include "openmc/thermal.h"

#include "xtensor/xbuilder.hpp"
#include "xtensor/xview.hpp"

#include <algorithm> // for sort
#include <string> // for to_string, stoi

namespace openmc {

//==============================================================================
// Global variables
//==============================================================================

namespace data {
std::array<double, 2> energy_min {0.0, 0.0};
std::array<double, 2> energy_max {INFTY, INFTY};
std::vector<std::unique_ptr<Nuclide>> nuclides;
} // namespace data

namespace simulation {
NuclideMicroXS* micro_xs;
MaterialMacroXS material_xs;
} // namespace simulation

//==============================================================================
// Nuclide implementation
//==============================================================================

int Nuclide::XS_TOTAL {0};
int Nuclide::XS_ABSORPTION {1};
int Nuclide::XS_FISSION {2};
int Nuclide::XS_NU_FISSION {3};
int Nuclide::XS_PHOTON_PROD {4};

Nuclide::Nuclide(hid_t group, const double* temperature, int n, int i_nuclide)
  : i_nuclide_{i_nuclide}
{
  // Get name of nuclide from group, removing leading '/'
  name_ = object_name(group).substr(1);

  read_attribute(group, "Z", Z_);
  read_attribute(group, "A", A_);
  read_attribute(group, "metastable", metastable_);
  read_attribute(group, "atomic_weight_ratio", awr_);

  // Determine temperatures available
  hid_t kT_group = open_group(group, "kTs");
  auto dset_names = dataset_names(kT_group);
  std::vector<double> temps_available;
  for (const auto& name : dset_names) {
    double T;
    read_dataset(kT_group, name.c_str(), T);
    temps_available.push_back(T / K_BOLTZMANN);
  }
  std::sort(temps_available.begin(), temps_available.end());

  // If only one temperature is available, revert to nearest temperature
  if (temps_available.size() == 1 && settings::temperature_method == TEMPERATURE_INTERPOLATION) {
    if (mpi::master) {
      warning("Cross sections for " + name_ + " are only available at one "
        "temperature. Reverting to nearest temperature method.");
    }
    settings::temperature_method = TEMPERATURE_NEAREST;
  }

  // Determine actual temperatures to read -- start by checking whether a
  // temperature range was given, in which case all temperatures in the range
  // are loaded irrespective of what temperatures actually appear in the model
  std::vector<int> temps_to_read;
  double T_min = n > 0 ? settings::temperature_range[0] : 0.0;
  double T_max = n > 0 ? settings::temperature_range[1] : INFTY;
  if (T_max > 0.0) {
    for (auto T : temps_available) {
      if (T_min <= T && T <= T_max) {
        temps_to_read.push_back(std::round(T));
      }
    }
  }

  switch (settings::temperature_method) {
  case TEMPERATURE_NEAREST:
    // Find nearest temperatures
    for (int i = 0; i < n; ++i) {
      double T_desired = temperature[i];

      // Determine closest temperature
      double min_delta_T = INFTY;
      double T_actual;
      for (auto T : temps_available) {
        double delta_T = std::abs(T - T_desired);
        if (delta_T < min_delta_T) {
          T_actual = T;
          min_delta_T = delta_T;
        }
      }

      if (std::abs(T_actual - T_desired) < settings::temperature_tolerance) {
        if (!contains(temps_to_read, std::round(T_actual))) {
          temps_to_read.push_back(std::round(T_actual));

          // Write warning for resonance scattering data if 0K is not available
          if (std::abs(T_actual - T_desired) > 0 && T_desired == 0 && mpi::master) {
            warning(name_ + " does not contain 0K data needed for resonance "
              "scattering options selected. Using data at " + std::to_string(T_actual)
              + " K instead.");
          }
        }
      } else {
        fatal_error("Nuclear data library does not contain cross sections for " +
          name_ + " at or near " + std::to_string(T_desired) + " K.");
      }
    }
    break;

  case TEMPERATURE_INTERPOLATION:
    // If temperature interpolation or multipole is selected, get a list of
    // bounding temperatures for each actual temperature present in the model
    for (int i = 0; i < n; ++i) {
      double T_desired = temperature[i];

      bool found_pair = false;
      for (int j = 0; j < temps_available.size() - 1; ++j) {
        if (temps_available[j] <= T_desired && T_desired < temps_available[j + 1]) {
          int T_j = std::round(temps_available[j]);
          int T_j1 = std::round(temps_available[j+1]);
          if (!contains(temps_to_read, T_j)) {
            temps_to_read.push_back(T_j);
          }
          if (!contains(temps_to_read, T_j1)) {
            temps_to_read.push_back(T_j1);
          }
          found_pair = true;
        }
      }

      if (!found_pair) {
        fatal_error("Nuclear data library does not contain cross sections for " +
          name_ +" at temperatures that bound " + std::to_string(T_desired) + " K.");
      }
    }
    break;
  }

  // Sort temperatures to read
  std::sort(temps_to_read.begin(), temps_to_read.end());

  hid_t energy_group = open_group(group, "energy");
  for (const auto& T : temps_to_read) {
    std::string dset {std::to_string(T) + "K"};

    // Determine exact kT values
    double kT;
    read_dataset(kT_group, dset.c_str(), kT);
    kTs_.push_back(kT);

    // Read energy grid
    grid_.emplace_back();
    read_dataset(energy_group, dset.c_str(), grid_.back().energy);
  }
  close_group(kT_group);

  // Check for 0K energy grid
  if (object_exists(energy_group, "0K")) {
    read_dataset(energy_group, "0K", energy_0K_);
  }
  close_group(energy_group);

  // Read reactions
  hid_t rxs_group = open_group(group, "reactions");
  for (auto name : group_names(rxs_group)) {
    if (starts_with(name, "reaction_")) {
      hid_t rx_group = open_group(rxs_group, name.c_str());
      reactions_.push_back(std::make_unique<Reaction>(rx_group, temps_to_read));

      // Check for 0K elastic scattering
      const auto& rx = reactions_.back();
      if (rx->mt_ == ELASTIC) {
        if (object_exists(rx_group, "0K")) {
          hid_t temp_group = open_group(rx_group, "0K");
          read_dataset(temp_group, "xs", elastic_0K_);
          close_group(temp_group);
        }
      }
      close_group(rx_group);

      // Determine reaction indices for inelastic scattering reactions
      if (is_inelastic_scatter(rx->mt_) && !rx->redundant_) {
        index_inelastic_scatter_.push_back(reactions_.size() - 1);
      }
    }
  }
  close_group(rxs_group);

  // Read unresolved resonance probability tables if present
  if (object_exists(group, "urr")) {
    urr_present_ = true;
    urr_data_.reserve(temps_to_read.size());

    for (int i = 0; i < temps_to_read.size(); i++) {
      // Get temperature as a string
      std::string temp_str {std::to_string(temps_to_read[i]) + "K"};

      // Read probability tables for i-th temperature
      hid_t urr_group = open_group(group, ("urr/" + temp_str).c_str());
      urr_data_.emplace_back(urr_group);
      close_group(urr_group);

      // Check for negative values
      if (xt::any(urr_data_[i].prob_ < 0.) && mpi::master) {
        warning("Negative value(s) found on probability table for nuclide " +
                name_ + " at " + temp_str);
      }
    }

    // If the inelastic competition flag indicates that the inelastic cross
    // section should be determined from a normal reaction cross section, we
    // need to get the index of the reaction.
    if (temps_to_read.size() > 0) {
      if (urr_data_[0].inelastic_flag_ > 0) {
        for (int i = 0; i < reactions_.size(); i++) {
          if (reactions_[i]->mt_ == urr_data_[0].inelastic_flag_) {
            urr_inelastic_ = i;
          }
        }

        // Abort if no corresponding inelastic reaction was found
        if (urr_inelastic_ == C_NONE) {
          fatal_error("Could no find inelastic reaction specified on "
                      "unresolved resonance probability table.");
        }
      }
    }
  }

  // Check for total nu data
  if (object_exists(group, "total_nu")) {
    // Read total nu data
    hid_t nu_group = open_group(group, "total_nu");
    total_nu_ = read_function(nu_group, "yield");
    close_group(nu_group);
  }

  // Read fission energy release data if present
  if (object_exists(group, "fission_energy_release")) {
    hid_t fer_group = open_group(group, "fission_energy_release");
    fission_q_prompt_ = read_function(fer_group, "q_prompt");
    fission_q_recov_ = read_function(fer_group, "q_recoverable");
    close_group(fer_group);
  }

  this->create_derived();
}

void Nuclide::create_derived()
{
  for (const auto& grid : grid_) {
    // Allocate and initialize cross section
    std::array<size_t, 2> shape {grid.energy.size(), 5};
    xs_.emplace_back(shape, 0.0);
  }

  reaction_index_.fill(-1);
  for (int i = 0; i < reactions_.size(); ++i) {
    const auto& rx {reactions_[i]};

    // Set entry in direct address table for reaction
    reaction_index_[rx->mt_] = i;

    for (int t = 0; t < kTs_.size(); ++t) {
      // TODO: off-by-one
      int j = rx->xs_[t].threshold - 1;
      int n = rx->xs_[t].value.size();
      auto xs = xt::adapt(rx->xs_[t].value);

      for (const auto& p : rx->products_) {
        if (p.particle_ == ParticleType::photon) {
          auto pprod = xt::view(xs_[t], xt::range(j, j+n), XS_PHOTON_PROD);
          for (int k = 0; k < n; ++k) {
            double E = grid_[t].energy[k+j];
            pprod[k] += xs[k] * (*p.yield_)(E);
          }
        }
      }

      // Skip redundant reactions
      if (rx->redundant_) continue;

      // Add contribution to total cross section
      auto total = xt::view(xs_[t], xt::range(j,j+n), XS_TOTAL);
      total += xs;

      // Add contribution to absorption cross section
      auto absorption = xt::view(xs_[t], xt::range(j,j+n), XS_ABSORPTION);
      if (is_disappearance(rx->mt_)) {
        absorption += xs;
      }

      if (is_fission(rx->mt_)) {
        fissionable_ = true;
        auto fission = xt::view(xs_[t], xt::range(j,j+n), XS_FISSION);
        fission += xs;
        absorption += xs;

        // Keep track of fission reactions
        if (t == 0) {
          fission_rx_.push_back(rx.get());
          if (rx->mt_ == N_F) has_partial_fission_ = true;
        }
      }
    }
  }

  // Determine number of delayed neutron precursors
  if (fissionable_) {
    for (const auto& product : fission_rx_[0]->products_) {
      if (product.emission_mode_ == EmissionMode::delayed) {
        ++n_precursor_;
      }
    }
  }

  // Calculate nu-fission cross section
  for (int t = 0; t < kTs_.size(); ++t) {
    if (fissionable_) {
      int n = grid_[t].energy.size();
      for (int i = 0; i < n; ++i) {
        double E = grid_[t].energy[i];
        xs_[t](i, XS_NU_FISSION) = nu(E, EmissionMode::total)
          * xs_[t](i, XS_FISSION);
      }
    }
  }

  if (settings::res_scat_on) {
    // Determine if this nuclide should be treated as a resonant scatterer
    if (!settings::res_scat_nuclides.empty()) {
      // If resonant nuclides were specified, check the list explicitly
      for (const auto& name : settings::res_scat_nuclides) {
        if (name_ == name) {
          resonant_ = true;

          // Make sure nuclide has 0K data
          if (energy_0K_.empty()) {
            fatal_error("Cannot treat " + name_ + " as a resonant scatterer "
              "because 0 K elastic scattering data is not present.");
          }
          break;
        }
      }
    } else {
      // Otherwise, assume that any that have 0 K elastic scattering data are
      // resonant
      resonant_ = !energy_0K_.empty();
    }

    if (resonant_) {
      // Build CDF for 0K elastic scattering
      double xs_cdf_sum = 0.0;
      xs_cdf_.resize(energy_0K_.size());
      xs_cdf_[0] = 0.0;

      const auto& E = energy_0K_;
      auto& xs = elastic_0K_;
      for (int i = 0; i < E.size() - 1; ++i) {
        // Negative cross sections result in a CDF that is not monotonically
        // increasing. Set all negative xs values to zero.
        if (xs[i] < 0.0) xs[i] = 0.0;

        // build xs cdf
        xs_cdf_sum += (std::sqrt(E[i])*xs[i] + std::sqrt(E[i+1])*xs[i+1])
              / 2.0 * (E[i+1] - E[i]);
        xs_cdf_[i] = xs_cdf_sum;
      }
    }
  }
}

void Nuclide::init_grid()
{
  int neutron = static_cast<int>(ParticleType::neutron) - 1;
  double E_min = data::energy_min[neutron];
  double E_max = data::energy_max[neutron];
  int M = settings::n_log_bins;

  // Determine equal-logarithmic energy spacing
  double spacing = std::log(E_max/E_min)/M;

  // Create equally log-spaced energy grid
  auto umesh = xt::linspace(0.0, M*spacing, M+1);

  for (auto& grid : grid_) {
    // Resize array for storing grid indices
    grid.grid_index.resize(M + 1);

    // Determine corresponding indices in nuclide grid to energies on
    // equal-logarithmic grid
    int j = 0;
    for (int k = 0; k <= M; ++k) {
      while (std::log(grid.energy[j + 1]/E_min) <= umesh(k)) {
        // Ensure that for isotopes where maxval(grid.energy) << E_max that
        // there are no out-of-bounds issues.
        if (j + 1 == grid.energy.size()) break;
        ++j;
      }
      grid.grid_index[k] = j;
    }
  }
}

double Nuclide::nu(double E, EmissionMode mode, int group) const
{
  if (!fissionable_) return 0.0;

  switch (mode) {
  case EmissionMode::prompt:
    return (*fission_rx_[0]->products_[0].yield_)(E);
  case EmissionMode::delayed:
    if (n_precursor_ > 0) {
      auto rx = fission_rx_[0];
      if (group >= 1 && group < rx->products_.size()) {
        // If delayed group specified, determine yield immediately
        return (*rx->products_[group].yield_)(E);
      } else {
        double nu {0.0};

        for (int i = 1; i < rx->products_.size(); ++i) {
          // Skip any non-neutron products
          const auto& product = rx->products_[i];
          if (product.particle_ != ParticleType::neutron) continue;

          // Evaluate yield
          if (product.emission_mode_ == EmissionMode::delayed) {
            nu += (*product.yield_)(E);
          }
        }
        return nu;
      }
    } else {
      return 0.0;
    }
  case EmissionMode::total:
    if (total_nu_) {
      return (*total_nu_)(E);
    } else {
      return (*fission_rx_[0]->products_[0].yield_)(E);
    }
  }
}

void Nuclide::calculate_elastic_xs() const
{
  // Get temperature index, grid index, and interpolation factor
  auto& micro = simulation::micro_xs[i_nuclide_];
  int i_temp = micro.index_temp;
  int i_grid = micro.index_grid - 1;
  double f = micro.interp_factor;

  if (i_temp >= 0) {
    const auto& xs = reactions_[0]->xs_[i_temp].value;
    micro.elastic = (1.0 - f)*xs[i_grid] + f*xs[i_grid + 1];
  }
}

double Nuclide::elastic_xs_0K(double E) const
{
  // Determine index on nuclide energy grid
  int i_grid;
  if (E < energy_0K_.front()) {
    i_grid = 0;
  } else if (E > energy_0K_.back()) {
    i_grid = energy_0K_.size() - 2;
  } else {
    i_grid = lower_bound_index(energy_0K_.begin(), energy_0K_.end(), E);
  }

  // check for rare case where two energy points are the same
  if (energy_0K_[i_grid] == energy_0K_[i_grid+1]) ++i_grid;

  // calculate interpolation factor
  double f = (E - energy_0K_[i_grid]) /
    (energy_0K_[i_grid + 1] - energy_0K_[i_grid]);

  // Calculate microscopic nuclide elastic cross section
  return (1.0 - f)*elastic_0K_[i_grid] + f*elastic_0K_[i_grid + 1];
}

void Nuclide::calculate_xs(int i_sab, double E, int i_log_union,
  double sqrtkT, double sab_frac)
{
  auto& micro_xs = simulation::micro_xs[i_nuclide_];

  // Initialize cached cross sections to zero
  micro_xs.elastic = CACHE_INVALID;
  micro_xs.thermal = 0.0;
  micro_xs.thermal_elastic = 0.0;

  // Check to see if there is multipole data present at this energy
  bool use_mp = false;
  if (multipole_) {
    use_mp = (E >= multipole_->E_min_ && E <= multipole_->E_max_);
  }

  // Evaluate multipole or interpolate
  if (use_mp) {
    // Call multipole kernel
    double sig_s, sig_a, sig_f;
    std::tie(sig_s, sig_a, sig_f) = multipole_->evaluate(E, sqrtkT);

    micro_xs.total = sig_s + sig_a;
    micro_xs.elastic = sig_s;
    micro_xs.absorption = sig_a;
    micro_xs.fission = sig_f;
    micro_xs.nu_fission = fissionable_ ?
      sig_f * this->nu(E, EmissionMode::total) : 0.0;

    if (simulation::need_depletion_rx) {
      // Only non-zero reaction is (n,gamma)
      micro_xs.reaction[0] = sig_a - sig_f;

      // Set all other reaction cross sections to zero
      for (int i = 1; i < DEPLETION_RX.size(); ++i) {
        micro_xs.reaction[i] = 0.0;
      }
    }

    // Ensure these values are set
    // Note, the only time either is used is in one of 4 places:
    // 1. physics.cpp - scatter - For inelastic scatter.
    // 2. physics.cpp - sample_fission - For partial fissions.
    // 3. tally.F90 - score_general - For tallying on MTxxx reactions.
    // 4. nuclide.cpp - calculate_urr_xs - For unresolved purposes.
    // It is worth noting that none of these occur in the resolved
    // resonance range, so the value here does not matter.  index_temp is
    // set to -1 to force a segfault in case a developer messes up and tries
    // to use it with multipole.
    micro_xs.index_temp = -1;
    micro_xs.index_grid = 0;
    micro_xs.interp_factor = 0.0;

  } else {
    // Find the appropriate temperature index.
    double kT = sqrtkT*sqrtkT;
    double f;
    int i_temp = -1;
    switch (settings::temperature_method) {
    case TEMPERATURE_NEAREST:
      {
        double max_diff = INFTY;
        for (int t = 0; t < kTs_.size(); ++t) {
          double diff = std::abs(kTs_[t] - kT);
          if (diff < max_diff) {
            i_temp = t;
            max_diff = diff;
          }
        }
      }
      break;

    case TEMPERATURE_INTERPOLATION:
      // Find temperatures that bound the actual temperature
      for (i_temp = 0; i_temp < kTs_.size() - 1; ++i_temp) {
        if (kTs_[i_temp] <= kT && kT < kTs_[i_temp + 1]) break;
      }

      // Randomly sample between temperature i and i+1
      f = (kT - kTs_[i_temp]) / (kTs_[i_temp + 1] - kTs_[i_temp]);
      if (f > prn()) ++i_temp;
      break;
    }

    // Determine the energy grid index using a logarithmic mapping to
    // reduce the energy range over which a binary search needs to be
    // performed

    const auto& grid {grid_[i_temp]};
    const auto& xs {xs_[i_temp]};

    int i_grid;
    if (E < grid.energy.front()) {
      i_grid = 0;
    } else if (E > grid.energy.back()) {
      i_grid = grid.energy.size() - 2;
    } else {
      // Determine bounding indices based on which equal log-spaced
      // interval the energy is in
      int i_low  = grid.grid_index[i_log_union];
      int i_high = grid.grid_index[i_log_union + 1] + 1;

      // Perform binary search over reduced range
      i_grid = i_low + lower_bound_index(&grid.energy[i_low], &grid.energy[i_high], E);
    }

    // check for rare case where two energy points are the same
    if (grid.energy[i_grid] == grid.energy[i_grid + 1]) ++i_grid;

    // calculate interpolation factor
    f = (E - grid.energy[i_grid]) /
      (grid.energy[i_grid + 1]- grid.energy[i_grid]);

    micro_xs.index_temp = i_temp;
    // TODO: off-by-one
    micro_xs.index_grid = i_grid + 1;
    micro_xs.interp_factor = f;

    // Calculate microscopic nuclide total cross section
    micro_xs.total = (1.0 - f)*xs(i_grid, XS_TOTAL)
          + f*xs(i_grid + 1, XS_TOTAL);

    // Calculate microscopic nuclide absorption cross section
    micro_xs.absorption = (1.0 - f)*xs(i_grid, XS_ABSORPTION)
      + f*xs(i_grid + 1, XS_ABSORPTION);

    if (fissionable_) {
      // Calculate microscopic nuclide total cross section
      micro_xs.fission = (1.0 - f)*xs(i_grid, XS_FISSION)
            + f*xs(i_grid + 1, XS_FISSION);

      // Calculate microscopic nuclide nu-fission cross section
      micro_xs.nu_fission = (1.0 - f)*xs(i_grid, XS_NU_FISSION)
        + f*xs(i_grid + 1, XS_NU_FISSION);
    } else {
      micro_xs.fission = 0.0;
      micro_xs.nu_fission = 0.0;
    }

    // Calculate microscopic nuclide photon production cross section
    micro_xs.photon_prod = (1.0 - f)*xs(i_grid, XS_PHOTON_PROD)
      + f*xs(i_grid + 1, XS_PHOTON_PROD);

    // Depletion-related reactions
    if (simulation::need_depletion_rx) {
      // Initialize all reaction cross sections to zero
      for (double& xs_i : micro_xs.reaction) {
        xs_i = 0.0;
      }

      for (int j = 0; j < DEPLETION_RX.size(); ++j) {
        // If reaction is present and energy is greater than threshold, set the
        // reaction xs appropriately
        int i_rx = reaction_index_[DEPLETION_RX[j]];
        if (i_rx >= 0) {
          const auto& rx = reactions_[i_rx];
          const auto& rx_xs = rx->xs_[i_temp].value;

          // Physics says that (n,gamma) is not a threshold reaction, so we don't
          // need to specifically check its threshold index
          if (j == 0) {
            micro_xs.reaction[0] = (1.0 - f)*rx_xs[i_grid]
              + f*rx_xs[i_grid + 1];
            continue;
          }

          // TODO: off-by-one
          int threshold = rx->xs_[i_temp].threshold - 1;
          if (i_grid >= threshold) {
            micro_xs.reaction[j] = (1.0 - f)*rx_xs[i_grid - threshold] +
              f*rx_xs[i_grid - threshold + 1];
          } else if (j >= 3) {
            // One can show that the the threshold for (n,(x+1)n) is always
            // higher than the threshold for (n,xn). Thus, if we are below
            // the threshold for, e.g., (n,2n), there is no reason to check
            // the threshold for (n,3n) and (n,4n).
            break;
          }
        }
      }
    }
  }

  // Initialize sab treatment to false
  micro_xs.index_sab = C_NONE;
  micro_xs.sab_frac = 0.0;

  // Initialize URR probability table treatment to false
  micro_xs.use_ptable = false;

  // If there is S(a,b) data for this nuclide, we need to set the sab_scatter
  // and sab_elastic cross sections and correct the total and elastic cross
  // sections.

  if (i_sab >= 0) this->calculate_sab_xs(i_sab, E, sqrtkT, sab_frac);

  // If the particle is in the unresolved resonance range and there are
  // probability tables, we need to determine cross sections from the table
  if (settings::urr_ptables_on && urr_present_ && !use_mp) {
    int n = urr_data_[micro_xs.index_temp].n_energy_;
    if ((E > urr_data_[micro_xs.index_temp].energy_(0)) &&
        (E < urr_data_[micro_xs.index_temp].energy_(n-1))) {
      this->calculate_urr_xs(micro_xs.index_temp, E);
    }
  }

  micro_xs.last_E = E;
  micro_xs.last_sqrtkT = sqrtkT;
}

void Nuclide::calculate_sab_xs(int i_sab, double E, double sqrtkT, double sab_frac)
{
  auto& micro {simulation::micro_xs[i_nuclide_]};

  // Set flag that S(a,b) treatment should be used for scattering
  micro.index_sab = i_sab;

  // Calculate the S(a,b) cross section
  int i_temp;
  double elastic;
  double inelastic;
  data::thermal_scatt[i_sab]->calculate_xs(E, sqrtkT, &i_temp, &elastic, &inelastic);

  // Store the S(a,b) cross sections.
  micro.thermal = sab_frac * (elastic + inelastic);
  micro.thermal_elastic = sab_frac * elastic;

  // Calculate free atom elastic cross section
  this->calculate_elastic_xs();

  // Correct total and elastic cross sections
  micro.total = micro.total + micro.thermal - sab_frac*micro.elastic;
  micro.elastic = micro.thermal + (1.0 - sab_frac)*micro.elastic;

  // Save temperature index and thermal fraction
  micro.index_temp_sab = i_temp;
  micro.sab_frac = sab_frac;
}

void Nuclide::calculate_urr_xs(int i_temp, double E) const
{
  auto& micro = simulation::micro_xs[i_nuclide_];
  micro.use_ptable = true;

  // Create a shorthand for the URR data
  const auto& urr = urr_data_[i_temp];

  // Determine the energy table
  int i_energy = 0;
  while (E >= urr.energy_(i_energy + 1)) {++i_energy;};

  // Sample the probability table using the cumulative distribution

  // Random nmbers for the xs calculation are sampled from a separate stream.
  // This guarantees the randomness and, at the same time, makes sure we
  // reuse random numbers for the same nuclide at different temperatures,
  // therefore preserving correlation of temperature in probability tables.
  prn_set_stream(STREAM_URR_PTABLE);
  //TODO: to maintain the same random number stream as the Fortran code this
  //replaces, the seed is set with i_nuclide_ + 1 instead of i_nuclide_
  double r = future_prn(static_cast<int64_t>(i_nuclide_ + 1));
  prn_set_stream(STREAM_TRACKING);

  int i_low = 0;
  while (urr.prob_(i_energy, URR_CUM_PROB, i_low) <= r) {++i_low;};

  int i_up = 0;
  while (urr.prob_(i_energy + 1, URR_CUM_PROB, i_up) <= r) {++i_up;};

  // Determine elastic, fission, and capture cross sections from the
  // probability table
  double elastic = 0.;
  double fission = 0.;
  double capture = 0.;
  double f;
  if (urr.interp_ == Interpolation::lin_lin) {
    // Determine the interpolation factor on the table
    f = (E - urr.energy_(i_energy)) /
         (urr.energy_(i_energy + 1) - urr.energy_(i_energy));

    elastic = (1. - f) * urr.prob_(i_energy, URR_ELASTIC, i_low) +
         f * urr.prob_(i_energy + 1, URR_ELASTIC, i_up);
    fission = (1. - f) * urr.prob_(i_energy, URR_FISSION, i_low) +
         f * urr.prob_(i_energy + 1, URR_FISSION, i_up);
    capture = (1. - f) * urr.prob_(i_energy, URR_N_GAMMA, i_low) +
         f * urr.prob_(i_energy + 1, URR_N_GAMMA, i_up);
  } else if (urr.interp_ == Interpolation::log_log) {
    // Determine interpolation factor on the table
    f = std::log(E / urr.energy_(i_energy)) /
         std::log(urr.energy_(i_energy + 1) / urr.energy_(i_energy));

    // Calculate the elastic cross section/factor
    if ((urr.prob_(i_energy, URR_ELASTIC, i_low) > 0.) &&
        (urr.prob_(i_energy + 1, URR_ELASTIC, i_up) > 0.)) {
      elastic =
           std::exp((1. - f) *
                    std::log(urr.prob_(i_energy, URR_ELASTIC, i_low)) +
                    f * std::log(urr.prob_(i_energy + 1, URR_ELASTIC, i_up)));
    } else {
      elastic = 0.;
    }

    // Calculate the fission cross section/factor
    if ((urr.prob_(i_energy, URR_FISSION, i_low) > 0.) &&
        (urr.prob_(i_energy + 1, URR_FISSION, i_up) > 0.)) {
      fission =
           std::exp((1. - f) *
                    std::log(urr.prob_(i_energy, URR_FISSION, i_low)) +
                    f * std::log(urr.prob_(i_energy + 1, URR_FISSION, i_up)));
    } else {
      fission = 0.;
    }

    // Calculate the capture cross section/factor
    if ((urr.prob_(i_energy, URR_N_GAMMA, i_low) > 0.) &&
        (urr.prob_(i_energy + 1, URR_N_GAMMA, i_up) > 0.)) {
      capture =
           std::exp((1. - f) *
                    std::log(urr.prob_(i_energy, URR_N_GAMMA, i_low)) +
                    f * std::log(urr.prob_(i_energy + 1, URR_N_GAMMA, i_up)));
    } else {
      capture = 0.;
    }
  }

  // Determine the treatment of inelastic scattering
  double inelastic = 0.;
  if (urr.inelastic_flag_ != C_NONE) {
    // get interpolation factor
    f = micro.interp_factor;

    // Determine inelastic scattering cross section
    Reaction* rx = reactions_[urr_inelastic_].get();
    int xs_index = micro.index_grid - rx->xs_[i_temp].threshold;
    if (xs_index >= 0) {
      inelastic = (1. - f) * rx->xs_[i_temp].value[xs_index] +
           f * rx->xs_[i_temp].value[xs_index + 1];
    }
  }

  // Multiply by smooth cross-section if needed
  if (urr.multiply_smooth_) {
    calculate_elastic_xs();
    elastic *= micro.elastic;
    capture *= (micro.absorption - micro.fission);
    fission *= micro.fission;
  }

  // Check for negative values
  if (elastic < 0.) {elastic = 0.;}
  if (fission < 0.) {fission = 0.;}
  if (capture < 0.) {capture = 0.;}

  // Set elastic, absorption, fission, and total x/s. Note that the total x/s
  // is calculated as a sum of partials instead of the table-provided value
  micro.elastic = elastic;
  micro.absorption = capture + fission;
  micro.fission = fission;
  micro.total = elastic + inelastic + capture + fission;

  // Determine nu-fission cross-section
  if (fissionable_) {
    micro.nu_fission = nu(E, EmissionMode::total) * micro.fission;
  }

}

//==============================================================================
// Fortran compatibility functions
//==============================================================================

extern "C" void
set_particle_energy_bounds(int particle, double E_min, double E_max)
{
  data::energy_min[particle - 1] = E_min;
  data::energy_max[particle - 1] = E_max;
}

extern "C" Nuclide* nuclide_from_hdf5_c(hid_t group, const double* temperature, int n)
{
  data::nuclides.push_back(std::make_unique<Nuclide>(group, temperature, n,
                                                     data::nuclides.size()));
  return data::nuclides.back().get();
}

extern "C" void nuclide_init_grid_c(Nuclide* nuc) { nuc->init_grid(); }

extern "C" Reaction* nuclide_reaction(Nuclide* nuc, int i_rx)
{
  return nuc->reactions_[i_rx-1].get();
}

extern "C" void nuclide_calculate_xs_c(Nuclide* nuc, int i_sab, double E,
  int i_log_union, double sqrtkT, double sab_frac)
{
  nuc->calculate_xs(i_sab, E, i_log_union, sqrtkT, sab_frac);
}

extern "C" void nuclide_calculate_elastic_xs_c(Nuclide* nuc)
{
  nuc->calculate_elastic_xs();
}

extern "C" double nuclide_nu_c(Nuclide* nuc, double E, int emission_mode, int group)
{
  return nuc->nu(E, static_cast<Nuclide::EmissionMode>(emission_mode - 1), group);
}

extern "C" double nuclide_fission_q_prompt(Nuclide* nuc, double E)
{
  return nuc->fission_q_prompt_ ? (*nuc->fission_q_prompt_)(E) : 0.0;
}

extern "C" double nuclide_fission_q_recov(Nuclide* nuc, double E)
{
  return nuc->fission_q_recov_ ? (*nuc->fission_q_recov_)(E) : 0.0;
}

extern "C" void nuclide_load_multipole(Nuclide* nuc, hid_t group)
{
  nuc->multipole_ = std::make_unique<WindowedMultipole>(group);
}

extern "C" void multipole_deriv_eval(Nuclide* nuc, double E, double sqrtkT,
  double* sig_s, double* sig_a, double* sig_f)
{
  std::tie(*sig_s, *sig_a, *sig_f) = nuc->multipole_->evaluate_deriv(E, sqrtkT);
}

extern "C" bool multipole_in_range(Nuclide* nuc, double E)
{
  return nuc->multipole_ && E >= nuc->multipole_->E_min_&&
    E <= nuc->multipole_->E_max_;
}

extern "C" void nuclides_clear() { data::nuclides.clear(); }

extern "C" NuclideMicroXS* micro_xs_ptr();

void set_micro_xs()
{
#pragma omp parallel
  {
    simulation::micro_xs = micro_xs_ptr();
  }
}

} // namespace openmc