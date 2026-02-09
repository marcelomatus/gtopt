/**
 * @file      osi_solver.hpp
 * @brief     Header of
 * @date      Mon Mar 24 09:51:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#pragma once

#ifdef COIN_FOUND

#ifdef COIN_USE_CPX
#  include <coin/OsiCpxSolverInterface.hpp>
#  define OSI_SOLVER CPLEX
using osiSolverInterface = OsiCpxSolverInterface;
#endif

#ifdef COIN_USE_CBC
#  include <coin/OsiCbcSolverInterface.hpp>
#  define OSI_SOLVER CBC
using osiSolverInterface = OsiCbcSolverInterface;
#endif

#ifndef OSI_SOLVER
#  ifndef COIN_USE_CLP
#    define COIN_USE_CLP
#  endif
#endif

#ifdef COIN_USE_CLP
#  include <coin/OsiClpSolverInterface.hpp>
#  define OSI_SOLVER CLP
// #  define OSI_EXTENDED
using osiSolverInterface = OsiClpSolverInterface;
#endif

#endif  // COIN_FOUND
