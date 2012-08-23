#ifndef _CPYP_CRP_H_
#define _CPYP_CRP_H_

#include <iostream>
#include <numeric>
#include <cassert>
#include <cmath>
#include <utility>
#include <unordered_map>
#include <functional>
#include "random.h"
#include "slice_sampler.h"
#include "crp_table_manager.h"
#include "m.h"

namespace cpyp {

// Chinese restaurant process (Pitman-Yor parameters) histogram-based table tracking
// based on the implementation proposed by Blunsom et al. 2009
//
// this implementation assumes that the observation likelihoods are either 1 (if they
// are identical to the "parameter" drawn from G_0) or 0. This is fine for most NLP
// applications but violated in PYP mixture models etc.
template <typename Dish, typename DishHash = std::hash<Dish> >
class crp {
 public:
  crp(double disc, double strength) :
      num_tables_(),
      num_customers_(),
      discount_(disc),
      strength_(strength),
      discount_prior_strength_(std::numeric_limits<double>::quiet_NaN()),
      discount_prior_beta_(std::numeric_limits<double>::quiet_NaN()),
      strength_prior_shape_(std::numeric_limits<double>::quiet_NaN()),
      strength_prior_rate_(std::numeric_limits<double>::quiet_NaN()) {
    check_hyperparameters();
  }

  crp(double d_strength, double d_beta, double c_shape, double c_rate, double d = 0.8, double c = 1.0) :
      num_tables_(),
      num_customers_(),
      discount_(d),
      strength_(c),
      discount_prior_strength_(d_strength),
      discount_prior_beta_(d_beta),
      strength_prior_shape_(c_shape),
      strength_prior_rate_(c_rate) {
    check_hyperparameters();
  }

  void check_hyperparameters() {
    if (discount_ < 0.0 || discount_ >= 1.0) {
      std::cerr << "Bad discount: " << discount_ << std::endl;
      abort();
    }
    if (strength_ <= -discount_) {
      std::cerr << "Bad strength: " << strength_ << " (discount=" << discount_ << ")" << std::endl;
      abort();
    }
  }

  double discount() const { return discount_; }
  double strength() const { return strength_; }
  void set_hyperparameters(double d, double s) {
    discount_ = d; strength_ = s;
    check_hyperparameters();
  }
  void set_discount(double d) { discount_ = d; check_hyperparameters(); }
  void set_strength(double a) { strength_ = a; check_hyperparameters(); }

  bool has_discount_prior() const {
    return !std::isnan(discount_prior_strength_);
  }

  bool has_strength_prior() const {
    return !std::isnan(strength_prior_shape_);
  }

  void clear() {
    num_tables_ = 0;
    num_customers_ = 0;
    dish_locs_.clear();
  }

  unsigned num_tables() const {
    return num_tables_;
  }

  unsigned num_tables(const Dish& dish) const {
    auto it = dish_locs_.find(dish);
    if (it == dish_locs_.end()) return 0;
    return it->second.num_tables();
  }

  unsigned num_customers() const {
    return num_customers_;
  }

  unsigned num_customers(const Dish& dish) const {
    auto it = dish_locs_.find(dish);
    if (it == dish_locs_.end()) return 0;
    return it->num_customers();
  }

  // returns +1 or 0 indicating whether a new table was opened
  //   p = probability with which the particular table was selected
  //       excluding p0
  template<typename F, typename Engine>
  int increment(const Dish& dish, const F& p0, Engine& eng) {
    crp_table_manager& loc = dish_locs_[dish];
    bool share_table = false;
    if (loc.num_customers()) {
      const F p_empty = F(strength_ + num_tables_ * discount_) * p0;
      const F p_share = F(loc.num_customers() - loc.num_tables() * discount_);
      share_table = sample_bernoulli(p_empty, p_share, eng);
    }
    if (share_table) {
      loc.share_table(discount_, eng);
    } else {
      loc.create_table();
      ++num_tables_;
    }
    ++num_customers_;
    return (share_table ? 0 : 1);
  }

  // returns -1 or 0, indicating whether a table was closed
  template<typename Engine>
  int decrement(const Dish& dish, Engine& eng) {
    crp_table_manager& loc = dish_locs_[dish];
    assert(loc.num_customers());
    if (loc.num_customers() == 1) {
      dish_locs_.erase(dish);
      --num_tables_;
      --num_customers_;
      return -1;
    } else {
      int delta = loc.remove_customer(eng);
      --num_customers_;
      if (delta) --num_tables_;
      return delta;
    }
  }

  template <typename F>
  F prob(const Dish& dish, const F& p0) const {
    auto it = dish_locs_.find(dish);
    const F r = F(num_tables_ * discount_ + strength_);
    if (it == dish_locs_.end()) {
      return r * p0 / F(num_customers_ + strength_);
    } else {
      return (F(it->second.num_customers() - discount_ * it->second.num_tables()) + r * p0) /
                   F(num_customers_ + strength_);
    }
  }

  double log_likelihood() const {
    return log_likelihood(discount_, strength_);
  }

  // taken from http://en.wikipedia.org/wiki/Chinese_restaurant_process
  // does not include P_0's
  double log_likelihood(const double& discount, const double& strength) const {
    double lp = 0.0;
    if (has_discount_prior())
      lp = Md::log_beta_density(discount, discount_prior_strength_, discount_prior_beta_);
    if (has_strength_prior())
      lp += Md::log_gamma_density(strength + discount, strength_prior_shape_, strength_prior_rate_);
    assert(lp <= 0.0);
    if (num_customers_) {  // if restaurant is not empty
      if (discount > 0.0) {  // two parameter case: discount > 0
        const double r = lgamma(1.0 - discount);
        if (strength)
          lp += lgamma(strength) - lgamma(strength / discount);
        lp += - lgamma(strength + num_customers_)
             + num_tables_ * log(discount) + lgamma(strength / discount + num_tables_);
        assert(std::isfinite(lp));
        for (auto& dish_loc : dish_locs_)
          for (auto& bin : dish_loc.second)
            lp += (lgamma(bin.first - discount) - r) * bin.second;
      } else if (!discount) { // discount == 0.0 (ie, Dirichlet Process)
        lp += lgamma(strength) + num_tables_ * log(strength) - lgamma(strength + num_tables_);
        assert(std::isfinite(lp));
        for (auto& dish_loc : dish_locs_)
          lp += lgamma(dish_loc.second.num_tables());
      } else { // should never happen
        assert(!"discount less than 0 detected!");
      }
    }
    assert(std::isfinite(lp));
    return lp;
  }

  template<typename Engine>
  void resample_hyperparameters(Engine& eng, const unsigned nloop = 5, const unsigned niterations = 10) {
    assert(has_discount_prior() || has_strength_prior());
    if (num_customers() == 0) return;
    for (unsigned iter = 0; iter < nloop; ++iter) {
      if (has_strength_prior()) {
        strength_ = slice_sampler1d([this](double prop_s) { return this->log_likelihood(discount_, prop_s); },
                               strength_, eng, -discount_ + std::numeric_limits<double>::min(),
                               std::numeric_limits<double>::infinity(), 0.0, niterations, 100*niterations);
      }
      if (has_discount_prior()) {
        double min_discount = std::numeric_limits<double>::min();
        if (strength_ < 0.0) min_discount -= strength_;
        discount_ = slice_sampler1d([this](double prop_d) { return this->log_likelihood(prop_d, strength_); },
                               discount_, eng, min_discount,
                               1.0, 0.0, niterations, 100*niterations);
      }
    }
    strength_ = slice_sampler1d([this](double prop_s) { return this->log_likelihood(discount_, prop_s); },
                             strength_, eng, -discount_,
                             std::numeric_limits<double>::infinity(), 0.0, niterations, 100*niterations);
  }

  void print(std::ostream* out) const {
    std::cerr << "PYP(d=" << discount_ << ",c=" << strength_ << ") customers=" << num_customers_ << std::endl;
    for (auto& dish_loc : dish_locs_)
      (*out) << dish_loc.first << " : " << dish_loc.second << std::endl;
  }

  typedef typename std::unordered_map<Dish, crp_table_manager, DishHash>::const_iterator const_iterator;
  const_iterator begin() const {
    return dish_locs_.begin();
  }
  const_iterator end() const {
    return dish_locs_.end();
  }

  void swap(crp<Dish>& b) {
    std::swap(num_tables_, b.num_tables_);
    std::swap(num_customers_, b.num_customers_);
    std::swap(dish_locs_, b.dish_locs_);
    std::swap(discount_, b.discount_);
    std::swap(strength_, b.strength_);
    std::swap(discount_prior_strength_, b.discount_prior_strength_);
    std::swap(discount_prior_beta_, b.discount_prior_beta_);
    std::swap(strength_prior_shape_, b.strength_prior_shape_);
    std::swap(strength_prior_rate_, b.strength_prior_rate_);
  }

 private:
  unsigned num_tables_;
  unsigned num_customers_;
  std::unordered_map<Dish, crp_table_manager, DishHash> dish_locs_;

  double discount_;
  double strength_;

  // optional beta prior on discount_ (NaN if no prior)
  double discount_prior_strength_;
  double discount_prior_beta_;

  // optional gamma prior on strength_ (NaN if no prior)
  double strength_prior_shape_;
  double strength_prior_rate_;
};

template<typename T>
void swap(crp<T>& a, crp<T>& b) {
  a.swap(b);
}

template <typename T,typename H>
std::ostream& operator<<(std::ostream& o, const crp<T,H>& c) {
  c.print(&o);
  return o;
}

}

#endif
