/**
 * @file      osi_solver.hpp
 * @brief     Solver back-end selection via COIN-OR Osi layer
 * @date      Mon Mar 24 09:51:39 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Selects the concrete OsiSolverInterface implementation based on the
 * compile-time COIN_USE_* macro set by cmake_local/Solver.cmake.
 * Priority: CPX > HGS > CBC > CLP (fallback).
 */

#pragma once

#ifdef COIN_USE_CPX
#  include <coin/OsiCpxSolverInterface.hpp>
#  define OSI_SOLVER CPLEX
using osiSolverInterface = OsiCpxSolverInterface;
#endif

#ifdef COIN_USE_HGS
#  include <OsiHiGHSSolverInterface.hpp>
#  define OSI_SOLVER HiGHS
using osiSolverInterface = OsiHiGHSSolverInterface;
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
