/* Copyright 2019 Luca Fedeli
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#ifndef WARPX_breit_wheeler_engine_wrapper_h_
#define WARPX_breit_wheeler_engine_wrapper_h_

#include "QedWrapperCommons.H"
#include "BreitWheelerEngineInnards.H"

#include <AMReX_Array.H>
#include <AMReX_Vector.H>
#include <AMReX_Gpu.H>

//#define PXRMP_CORE_ONLY allows importing only the 'core functions' of the
//Breit Wheeler engine of the QED PICSAR library.
#define PXRMP_CORE_ONLY
#include <breit_wheeler_engine.hpp>

//Lookup table building function is in a dedicated (optional) class to
//avoid including heavy dependencies if they are not needed.
#ifdef WARPX_QED_TABLE_GEN
#    include "BreitWheelerEngineTableBuilder.H"
#endif

#include <string>

//Some handy aliases

// The engine has two templated arguments: the numerical type
// and a random number generator. However, random numbers are not
// used to generate the lookup tables and the static member
// functions which are called in the functors do not use
// random numbers as well. Therefore, an empty "DummyStruct"
// can be passed as a template parameter.
using PicsarBreitWheelerEngine = picsar::multi_physics::
    breit_wheeler_engine<amrex::Real, QedUtils::DummyStruct>;

using PicsarBreitWheelerCtrl =
    picsar::multi_physics::breit_wheeler_engine_ctrl<amrex::Real>;
//__________

// Functors ==================================

// These functors allow using the core elementary functions of the library.
// They are generated by a factory class (BreitWheelerEngine, see below).
// They can be included in GPU kernels.

/**
 * Functor to initialize the optical depth of photons for the
 * Breit-Wheeler process
 */
class BreitWheelerGetOpticalDepth
{
public:
    /**
     * Constructor does nothing because optical depth initialization
     * does not require control parameters or lookup tables.
     */
    BreitWheelerGetOpticalDepth ()
    {};

    /**
     * () operator is just a thin wrapper around a very simple function to
     * generate the optical depth. It can be used on GPU.
     */
    AMREX_GPU_HOST_DEVICE
    AMREX_FORCE_INLINE
    amrex::Real operator() () const noexcept
    {
        //A random number in [0,1) should be provided as an argument.
        return PicsarBreitWheelerEngine::
            internal_get_optical_depth(amrex::Random());
    }
};
//____________________________________________

/**
 * Functor to evolve the optical depth of photons due to the
 * Breit-Wheeler process
 */
class BreitWheelerEvolveOpticalDepth
{
public:
    /**
     * Constructor acquires a reference to control parameters and
     * lookup tables data.
     * lookup_table uses non-owning vectors under the hood. So no new data
     * allocations should be triggered on GPU
     */
    BreitWheelerEvolveOpticalDepth(BreitWheelerEngineInnards& r_innards):
        m_ctrl{r_innards.ctrl},
        m_TTfunc_size{r_innards.TTfunc_coords.size()},
        m_p_TTfunc_coords{r_innards.TTfunc_coords.dataPtr()},
        m_p_TTfunc_data{r_innards.TTfunc_data.dataPtr()}
        {};

    /**
     * Evolves the optical depth. It can be used on GPU.
     * @param[in] px,py,pz momentum components of the photon (SI units)
     * @param[in] ex,ey,ez electric field components (SI units)
     * @param[in] bx,by,bz magnetic field components (SI units)
     * @param[in] dt timestep (SI units)
     * @param[in,out] opt_depth optical depth of the photon. It is modified by the method.
     * @return a flag which is 1 if optical depth becomes negative (i.e. a pair has to be generated).
     */
    AMREX_GPU_HOST_DEVICE
    AMREX_FORCE_INLINE
    int operator()(
    amrex::Real px, amrex::Real py, amrex::Real pz,
    amrex::Real ex, amrex::Real ey, amrex::Real ez,
    amrex::Real bx, amrex::Real by, amrex::Real bz,
    amrex::Real dt, amrex::Real& opt_depth) const noexcept
    {
        bool has_event_happened{false};

        //the library provides the time (< dt) at which the event occurs, but this
        //feature won't be used in WarpX for now.
        amrex::Real unused_event_time{0.0};

        PicsarBreitWheelerEngine::
        internal_evolve_opt_depth_and_determine_event(
            px, py, pz,
            ex, ey, ez,
            bx, by, bz,
            dt, opt_depth,
            has_event_happened, unused_event_time,
            m_dummy_lambda,
            picsar::multi_physics::lookup_1d<amrex::Real>{
                m_TTfunc_size,
                m_p_TTfunc_coords,
                m_p_TTfunc_data},
            m_ctrl);

        return has_event_happened;
    }

private:
    //laser wavelenght is not used with SI units
    const amrex::Real m_dummy_lambda{1.0};

    const PicsarBreitWheelerCtrl m_ctrl;

    //lookup table data
    size_t m_TTfunc_size;
    amrex::Real* m_p_TTfunc_coords;
    amrex::Real* m_p_TTfunc_data;
};

/**
 * Functor to generate a pair via the
 * Breit-Wheeler process
 */
class BreitWheelerGeneratePairs
{
public:
    /**
     * Constructor acquires pointers to control parameters and
     * lookup tables data.
     * lookup_table uses non-owning vectors under the hood. So no new data
     * allocations should be triggered on GPU
     */
    BreitWheelerGeneratePairs(
        BreitWheelerEngineInnards& r_innards):
        m_ctrl{r_innards.ctrl},
        m_cum_distrib_coords_1_size{r_innards.cum_distrib_coords_1.size()},
        m_cum_distrib_coords_2_size{r_innards.cum_distrib_coords_2.size()},
        m_p_distrib_coords_1{r_innards.cum_distrib_coords_1.data()},
        m_p_distrib_coords_2{r_innards.cum_distrib_coords_2.data()},
        m_p_cum_distrib_data{r_innards.cum_distrib_data.data()
        }{};

    /**
     * Generates sampling (template parameter) pairs according to Breit Wheeler process.
     * It can be used on GPU.
     * @param[in] px,py,pz momentum components of the photon (SI units)
     * @param[in] ex,ey,ez electric field components (SI units)
     * @param[in] bx,by,bz magnetic field components (SI units)
     * @param[in] weight of the photon (code units)
     * @param[out] e_px,e_py,e_pz momenta of generated electrons. Each array should have size=sampling (SI units)
     * @param[out] p_px,p_py,p_pz momenta of generated positrons. Each array should have size=sampling (SI units)
     * @param[out] e_weight,p_weight weight of the generated particles Each array should have size=sampling (code units).
     */
    template <size_t sampling>
    AMREX_GPU_HOST_DEVICE
    AMREX_FORCE_INLINE
    void operator()(
    amrex::Real px, amrex::Real py, amrex::Real pz,
    amrex::Real ex, amrex::Real ey, amrex::Real ez,
    amrex::Real bx, amrex::Real by, amrex::Real bz,
    amrex::Real weight,
    amrex::Real* e_px, amrex::Real* e_py, amrex::Real* e_pz,
    amrex::Real* p_px, amrex::Real* p_py, amrex::Real* p_pz,
    amrex::Real* e_weight, amrex::Real* p_weight) const noexcept
    {
        //[sampling] random numbers are needed
        picsar::multi_physics::picsar_array<amrex::Real, sampling>
            rand_zero_one_minus_epsi;
        for(auto& el : rand_zero_one_minus_epsi) el = amrex::Random();

        const auto p_rand = rand_zero_one_minus_epsi.data();

        PicsarBreitWheelerEngine::
        internal_generate_breit_wheeler_pairs(
            px, py, pz,
            ex, ey, ez,
            bx, by, bz,
            weight, sampling,
            e_px, e_py, e_pz,
            p_px, p_py, p_pz,
            e_weight, p_weight,
            m_dummy_lambda,
            picsar::multi_physics::lookup_2d<amrex::Real>{
                m_cum_distrib_coords_1_size,
                m_p_distrib_coords_1,
                m_cum_distrib_coords_2_size,
                m_p_distrib_coords_2,
                m_p_cum_distrib_data
            },
            m_ctrl,
            p_rand);
    }

private:
    //laser wavelenght is not used with SI units
    const amrex::Real m_dummy_lambda{1.0};

    const PicsarBreitWheelerCtrl m_ctrl;

    //lookup table data
    size_t m_cum_distrib_coords_1_size;
    size_t m_cum_distrib_coords_2_size;
    amrex::Real* m_p_distrib_coords_1;
    amrex::Real* m_p_distrib_coords_2;
    amrex::Real* m_p_cum_distrib_data;
};

// Factory class =============================

/**
 * Wrapper for the Breit Wheeler engine of the PICSAR library
 */
class BreitWheelerEngine
{
public:
    /**
     * Constructor requires no arguments.
     */
    BreitWheelerEngine ();

    /**
     * Builds the functor to initialize the optical depth
     */
    BreitWheelerGetOpticalDepth build_optical_depth_functor ();

    /**
     * Builds the functor to evolve the optical depth
     */
    BreitWheelerEvolveOpticalDepth build_evolve_functor ();

    /**
     * Builds the functor to generate the pairs
     */
    BreitWheelerGeneratePairs build_pair_functor ();

    /**
     * Checks if the optical tables are properly initialized
     */
    bool are_lookup_tables_initialized () const;

    /**
     * Init lookup tables from raw binary data.
     * @param[in] raw_data a Vector of char
     * @return true if it succeeds, false if it cannot parse raw_data
     */
    bool init_lookup_tables_from_raw_data (const amrex::Vector<char>& raw_data);

    /**
     * Init lookup tables using built-in dummy tables
     * for test purposes.
     */
    void init_dummy_tables();

    /**
     * Export lookup tables data into a raw binary Vector
     * @return the data in binary format. The Vector is empty if tables were
     * not previously initialized.
     */
    amrex::Vector<char> export_lookup_tables_data () const;

    /**
     * Computes the lookup tables. It does nothing unless WarpX is compiled with QED_TABLE_GEN=TRUE
     * @param[in] ctrl control params to generate the tables
     */
    void compute_lookup_tables (PicsarBreitWheelerCtrl ctrl);

    /**
     * gets default (reasonable) values for the control parameters
     * @return default control params to generate the tables
     */
    PicsarBreitWheelerCtrl get_default_ctrl() const;

    /**
     * returns a constant reference to the control parameters
     * @return const reference to control parameters
     */
    const PicsarBreitWheelerCtrl& get_ref_ctrl() const;

private:
    bool m_lookup_tables_initialized = false;

    BreitWheelerEngineInnards m_innards;

//Table builing is available only if WarpX is compiled with QED_TABLE_GEN=TRUE
#ifdef WARPX_QED_TABLE_GEN
    BreitWheelerEngineTableBuilder m_table_builder;
#endif

};

//============================================

#endif //WARPX_breit_wheeler_engine_wrapper_H_
