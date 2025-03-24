/**
 * @file      gtopt.hpp
 * @brief     Header of Block class
 * @date      Sun Mar 23 00:34:06 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Block class
 */

#pragma once

#include <string>

#include <gtopt/block.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/input_context.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/scenery_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_options.hpp>
#include <gtopt/system_options_lp.hpp>

namespace gtopt
{
/**  Language codes to be used with the Gtopt class */
enum class LanguageCode
{
  EN,
  DE,
  ES,
  FR
};

/**
 * @brief A class for saying hello in multiple languages
 */
class Gtopt
{
  std::string name;

public:
  /**
   * @brief Creates a new gtopt
   * @param name the name to greet
   */
  explicit Gtopt(std::string name);

  /**
   * @brief Creates a localized string containing the greeting
   * @param lang the language to greet in
   * @return a string containing the greeting
   */
  [[nodiscard]] std::string greet(LanguageCode lang = LanguageCode::EN) const;
};

}  // namespace gtopt
