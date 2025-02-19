/* Copyright 2019-2020 David Grote
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#include "PsatdAlgorithmRZ.H"
#include "Utils/WarpXConst.H"
#include "Utils/WarpXProfilerWrapper.H"
#include "WarpX.H"

#include <cmath>

using amrex::operator""_rt;


/* \brief Initialize coefficients for the update equation */
PsatdAlgorithmRZ::PsatdAlgorithmRZ (SpectralKSpaceRZ const & spectral_kspace,
                                    amrex::DistributionMapping const & dm,
                                    const SpectralFieldIndex& spectral_index,
                                    int const n_rz_azimuthal_modes, int const norder_z,
                                    bool const nodal, amrex::Real const dt,
                                    bool const update_with_rho,
                                    const bool time_averaging,
                                    const bool J_linear_in_time,
                                    const bool dive_cleaning,
                                    const bool divb_cleaning)
     // Initialize members of base class
     : SpectralBaseAlgorithmRZ(spectral_kspace, dm, spectral_index, norder_z, nodal),
       m_spectral_index(spectral_index),
       m_dt(dt),
       m_update_with_rho(update_with_rho),
       m_time_averaging(time_averaging),
       m_J_linear_in_time(J_linear_in_time),
       m_dive_cleaning(dive_cleaning),
       m_divb_cleaning(divb_cleaning)
{

    // Allocate the arrays of coefficients
    amrex::BoxArray const & ba = spectral_kspace.spectralspace_ba;
    C_coef = SpectralRealCoefficients(ba, dm, n_rz_azimuthal_modes, 0);
    S_ck_coef = SpectralRealCoefficients(ba, dm, n_rz_azimuthal_modes, 0);
    X1_coef = SpectralRealCoefficients(ba, dm, n_rz_azimuthal_modes, 0);
    X2_coef = SpectralRealCoefficients(ba, dm, n_rz_azimuthal_modes, 0);
    X3_coef = SpectralRealCoefficients(ba, dm, n_rz_azimuthal_modes, 0);

    coefficients_initialized = false;

    // TODO Implement time averaging and remove this
    amrex::ignore_unused(m_time_averaging);
}

/* Advance the E and B field in spectral space (stored in `f`)
 * over one time step */
void
PsatdAlgorithmRZ::pushSpectralFields(SpectralFieldDataRZ & f)
{

    bool const update_with_rho = m_update_with_rho;
    const bool J_linear_in_time = m_J_linear_in_time;
    const bool dive_cleaning = m_dive_cleaning;
    const bool divb_cleaning = m_divb_cleaning;

    if (not coefficients_initialized) {
        // This is called from here since it needs the kr values
        // which can be obtained from the SpectralFieldDataRZ
        InitializeSpectralCoefficients(f);
        coefficients_initialized = true;
    }

    const SpectralFieldIndex& Idx = m_spectral_index;

    // Loop over boxes
    for (amrex::MFIter mfi(f.fields); mfi.isValid(); ++mfi){

        amrex::Box const & bx = f.fields[mfi].box();

        // Extract arrays for the fields to be updated
        amrex::Array4<Complex> const& fields = f.fields[mfi].array();
        // Extract arrays for the coefficients
        amrex::Array4<const amrex::Real> const& C_arr = C_coef[mfi].array();
        amrex::Array4<const amrex::Real> const& S_ck_arr = S_ck_coef[mfi].array();
        amrex::Array4<const amrex::Real> const& X1_arr = X1_coef[mfi].array();
        amrex::Array4<const amrex::Real> const& X2_arr = X2_coef[mfi].array();
        amrex::Array4<const amrex::Real> const& X3_arr = X3_coef[mfi].array();

        // Extract pointers for the k vectors
        auto const & kr_modes = f.getKrArray(mfi);
        amrex::Real const* kr_arr = kr_modes.dataPtr();
        amrex::Real const* modified_kz_arr = modified_kz_vec[mfi].dataPtr();
        int const nr = bx.length(0);
        amrex::Real const dt = m_dt;

        // Loop over indices within one box
        // Note that k = 0
        int const modes = f.n_rz_azimuthal_modes;
        amrex::ParallelFor(bx, modes,
        [=] AMREX_GPU_DEVICE(int i, int j, int k, int mode) noexcept
        {

            // All of the fields of each mode are grouped together
            int const Ep_m = Idx.Ex + Idx.n_fields*mode;
            int const Em_m = Idx.Ey + Idx.n_fields*mode;
            int const Ez_m = Idx.Ez + Idx.n_fields*mode;
            int const Bp_m = Idx.Bx + Idx.n_fields*mode;
            int const Bm_m = Idx.By + Idx.n_fields*mode;
            int const Bz_m = Idx.Bz + Idx.n_fields*mode;
            int const Jp_m = Idx.Jx + Idx.n_fields*mode;
            int const Jm_m = Idx.Jy + Idx.n_fields*mode;
            int const Jz_m = Idx.Jz + Idx.n_fields*mode;
            int const rho_old_m = Idx.rho_old + Idx.n_fields*mode;
            int const rho_new_m = Idx.rho_new + Idx.n_fields*mode;

            // Record old values of the fields to be updated
            Complex const Ep_old = fields(i,j,k,Ep_m);
            Complex const Em_old = fields(i,j,k,Em_m);
            Complex const Ez_old = fields(i,j,k,Ez_m);
            Complex const Bp_old = fields(i,j,k,Bp_m);
            Complex const Bm_old = fields(i,j,k,Bm_m);
            Complex const Bz_old = fields(i,j,k,Bz_m);
            // Shortcut for the values of J and rho
            Complex const Jp = fields(i,j,k,Jp_m);
            Complex const Jm = fields(i,j,k,Jm_m);
            Complex const Jz = fields(i,j,k,Jz_m);
            Complex const rho_old = fields(i,j,k,rho_old_m);
            Complex const rho_new = fields(i,j,k,rho_new_m);

            // k vector values, and coefficients
            // The k values for each mode are grouped together
            int const ir = i + nr*mode;
            amrex::Real const kr = kr_arr[ir];
            amrex::Real const kz = modified_kz_arr[j];

            constexpr amrex::Real c2 = PhysConst::c*PhysConst::c;
            constexpr amrex::Real inv_ep0 = 1._rt/PhysConst::ep0;
            Complex const I = Complex{0._rt,1._rt};
            amrex::Real const C = C_arr(i,j,k,mode);
            amrex::Real const S_ck = S_ck_arr(i,j,k,mode);
            amrex::Real const X1 = X1_arr(i,j,k,mode);
            amrex::Real const X2 = X2_arr(i,j,k,mode);
            amrex::Real const X3 = X3_arr(i,j,k,mode);

            Complex rho_diff;
            if (update_with_rho) {
                rho_diff = X2*rho_new - X3*rho_old;
            } else {
                Complex const divE = kr*(Ep_old - Em_old) + I*kz*Ez_old;
                Complex const divJ = kr*(Jp - Jm) + I*kz*Jz;

                rho_diff = (X2 - X3)*PhysConst::ep0*divE - X2*dt*divJ;
            }

            // Update E (see WarpX online documentation: theory section)
            fields(i,j,k,Ep_m) = C*Ep_old
                        + S_ck*(-c2*I*kr/2._rt*Bz_old + c2*kz*Bp_old - inv_ep0*Jp)
                        + 0.5_rt*kr*rho_diff;
            fields(i,j,k,Em_m) = C*Em_old
                        + S_ck*(-c2*I*kr/2._rt*Bz_old - c2*kz*Bm_old - inv_ep0*Jm)
                        - 0.5_rt*kr*rho_diff;
            fields(i,j,k,Ez_m) = C*Ez_old
                        + S_ck*(c2*I*kr*Bp_old + c2*I*kr*Bm_old - inv_ep0*Jz)
                        - I*kz*rho_diff;
            // Update B (see WarpX online documentation: theory section)
            fields(i,j,k,Bp_m) = C*Bp_old
                        - S_ck*(-I*kr/2._rt*Ez_old + kz*Ep_old)
                        + X1*(-I*kr/2._rt*Jz + kz*Jp);
            fields(i,j,k,Bm_m) = C*Bm_old
                        - S_ck*(-I*kr/2._rt*Ez_old - kz*Em_old)
                        + X1*(-I*kr/2._rt*Jz - kz*Jm);
            fields(i,j,k,Bz_m) = C*Bz_old
                        - S_ck*I*(kr*Ep_old + kr*Em_old)
                        + X1*I*(kr*Jp + kr*Jm);

            int F_m;
            Complex F_old;
            if (dive_cleaning)
            {
                F_m = Idx.F + Idx.n_fields*mode;
                F_old = fields(i,j,k,F_m);
            }

            int G_m;
            Complex G_old;
            if (divb_cleaning)
            {
                G_m = Idx.G + Idx.n_fields*mode;
                G_old = fields(i,j,k,G_m);
            }

            if (J_linear_in_time)
            {
                const int Jp_m_new = Idx.Jx_new + Idx.n_fields*mode;
                const int Jm_m_new = Idx.Jy_new + Idx.n_fields*mode;
                const int Jz_m_new = Idx.Jz_new + Idx.n_fields*mode;

                const Complex Jp_new = fields(i,j,k,Jp_m_new);
                const Complex Jm_new = fields(i,j,k,Jm_m_new);
                const Complex Jz_new = fields(i,j,k,Jz_m_new);

                fields(i,j,k,Ep_m) += -X1 * (Jp_new - Jp) / dt;
                fields(i,j,k,Em_m) += -X1 * (Jm_new - Jm) / dt;
                fields(i,j,k,Ez_m) += -X1 * (Jz_new - Jz) / dt;

                fields(i,j,k,Bp_m) +=  X2/c2 * (kz * (Jp_new - Jp) - I * kr * 0.5_rt * (Jz_new - Jz));
                fields(i,j,k,Bm_m) += -X2/c2 * (kz * (Jm_new - Jm) + I * kr * 0.5_rt * (Jz_new - Jz));
                fields(i,j,k,Bz_m) += I * X2/c2 * (kr * (Jp_new - Jp) + kr * (Jm_new - Jm));

                if (dive_cleaning)
                {
                    const Complex k_dot_J  = -I * (kr * (Jp - Jm) + I * kz * Jz);
                    const Complex k_dot_dJ = -I * (kr * ((Jp_new - Jp) - (Jm_new - Jm))
                        + I * kz * (Jz_new - Jz));
                    const Complex k_dot_E = -I * (kr * (Ep_old - Em_old) + I * kz * Ez_old);

                    fields(i,j,k,Ep_m) += -c2 * kr * 0.5_rt * S_ck * F_old;
                    fields(i,j,k,Em_m) +=  c2 * kr * 0.5_rt * S_ck * F_old;
                    fields(i,j,k,Ez_m) += I * c2 * kz * S_ck * F_old;

                    fields(i,j,k,F_m) = C * F_old + S_ck * (I * k_dot_E - inv_ep0 * rho_old)
                        - X1 * ((rho_new - rho_old) / dt + I * k_dot_J) - I * X2/c2 * k_dot_dJ;
                }

                if (divb_cleaning)
                {
                    const Complex k_dot_B = -I * (kr * (Bp_old - Bm_old) + I * kz * Bz_old);

                    fields(i,j,k,Bp_m) += -c2 * kr * 0.5_rt * S_ck * G_old;
                    fields(i,j,k,Bm_m) +=  c2 * kr * 0.5_rt * S_ck * G_old;
                    fields(i,j,k,Bz_m) += I * c2 * kz * S_ck * G_old;

                    fields(i,j,k,G_m) = C * G_old + I * S_ck * k_dot_B;
                }
            }
        });
    }
}

void PsatdAlgorithmRZ::InitializeSpectralCoefficients (SpectralFieldDataRZ const & f)
{

    // Fill them with the right values:
    // Loop over boxes and allocate the corresponding coefficients
    // for each box owned by the local MPI proc
    for (amrex::MFIter mfi(f.fields); mfi.isValid(); ++mfi){

        amrex::Box const & bx = f.fields[mfi].box();

        // Extract pointers for the k vectors
        amrex::Real const* const modified_kz = modified_kz_vec[mfi].dataPtr();

        // Extract arrays for the coefficients
        amrex::Array4<amrex::Real> const& C = C_coef[mfi].array();
        amrex::Array4<amrex::Real> const& S_ck = S_ck_coef[mfi].array();
        amrex::Array4<amrex::Real> const& X1 = X1_coef[mfi].array();
        amrex::Array4<amrex::Real> const& X2 = X2_coef[mfi].array();
        amrex::Array4<amrex::Real> const& X3 = X3_coef[mfi].array();

        auto const & kr_modes = f.getKrArray(mfi);
        amrex::Real const* kr_arr = kr_modes.dataPtr();
        int const nr = bx.length(0);
        amrex::Real const dt = m_dt;

        // Loop over indices within one box
        int const modes = f.n_rz_azimuthal_modes;
        amrex::ParallelFor(bx, modes,
        [=] AMREX_GPU_DEVICE(int i, int j, int k, int mode) noexcept
        {
            // Calculate norm of vector
            int const ir = i + nr*mode;
            amrex::Real const kr = kr_arr[ir];
            amrex::Real const kz = modified_kz[j];
            amrex::Real const k_norm = std::sqrt(kr*kr + kz*kz);

            // Calculate coefficients
            constexpr amrex::Real c = PhysConst::c;
            constexpr amrex::Real ep0 = PhysConst::ep0;
            if (k_norm != 0){
                C(i,j,k,mode) = std::cos(c*k_norm*dt);
                S_ck(i,j,k,mode) = std::sin(c*k_norm*dt)/(c*k_norm);
                X1(i,j,k,mode) = (1._rt - C(i,j,k,mode))/(ep0 * c*c * k_norm*k_norm);
                X2(i,j,k,mode) = (1._rt - S_ck(i,j,k,mode)/dt)/(ep0 * k_norm*k_norm);
                X3(i,j,k,mode) = (C(i,j,k,mode) - S_ck(i,j,k,mode)/dt)/(ep0 * k_norm*k_norm);
            } else { // Handle k_norm = 0, by using the analytical limit
                C(i,j,k,mode) = 1._rt;
                S_ck(i,j,k,mode) = dt;
                X1(i,j,k,mode) = 0.5_rt * dt*dt / ep0;
                X2(i,j,k,mode) = c*c * dt*dt / (6._rt*ep0);
                X3(i,j,k,mode) = - c*c * dt*dt / (3._rt*ep0);
            }
        });
     }
}

void
PsatdAlgorithmRZ::CurrentCorrection (const int lev,
                                     SpectralFieldDataRZ& field_data,
                                     std::array<std::unique_ptr<amrex::MultiFab>,3>& current,
                                     const std::unique_ptr<amrex::MultiFab>& rho)
{
    // Profiling
    WARPX_PROFILE( "PsatdAlgorithmRZ::CurrentCorrection" );

    const SpectralFieldIndex& Idx = m_spectral_index;

    // Forward Fourier transform of J and rho
    field_data.ForwardTransform( lev,
                                 *current[0], Idx.Jx,
                                 *current[1], Idx.Jy);
    field_data.ForwardTransform( lev, *current[2], Idx.Jz, 0);
    field_data.ForwardTransform( lev, *rho, Idx.rho_old, 0 );
    field_data.ForwardTransform( lev, *rho, Idx.rho_new, 1 );

    // Loop over boxes
    for (amrex::MFIter mfi(field_data.fields); mfi.isValid(); ++mfi){

        amrex::Box const & bx = field_data.fields[mfi].box();

        // Extract arrays for the fields to be updated
        amrex::Array4<Complex> fields = field_data.fields[mfi].array();

        // Extract pointers for the k vectors
        auto const & kr_modes = field_data.getKrArray(mfi);
        amrex::Real const* kr_arr = kr_modes.dataPtr();
        amrex::Real const* modified_kz_arr = modified_kz_vec[mfi].dataPtr();
        int const nr = bx.length(0);

        // Local copy of member variables before GPU loop
        amrex::Real const dt = m_dt;

        // Loop over indices within one box
        int const modes = field_data.n_rz_azimuthal_modes;
        ParallelFor(bx, modes,
        [=] AMREX_GPU_DEVICE(int i, int j, int k, int mode) noexcept
        {
            // All of the fields of each mode are grouped together
            auto const Jp_m = Idx.Jx + Idx.n_fields*mode;
            auto const Jm_m = Idx.Jy + Idx.n_fields*mode;
            auto const Jz_m = Idx.Jz + Idx.n_fields*mode;
            auto const rho_old_m = Idx.rho_old + Idx.n_fields*mode;
            auto const rho_new_m = Idx.rho_new + Idx.n_fields*mode;

            // Shortcuts for the values of J and rho
            Complex const Jp = fields(i,j,k,Jp_m);
            Complex const Jm = fields(i,j,k,Jm_m);
            Complex const Jz = fields(i,j,k,Jz_m);
            Complex const rho_old = fields(i,j,k,rho_old_m);
            Complex const rho_new = fields(i,j,k,rho_new_m);

            // k vector values, and coefficients
            // The k values for each mode are grouped together
            int const ir = i + nr*mode;
            amrex::Real const kr = kr_arr[ir];
            amrex::Real const kz = modified_kz_arr[j];
            amrex::Real const k_norm2 = kr*kr + kz*kz;

            constexpr Complex I = Complex{0._rt,1._rt};

            // Correct J
            if ( k_norm2 != 0._rt )
            {
                Complex const F = - ((rho_new - rho_old)/dt + I*kz*Jz + kr*(Jp - Jm))/k_norm2;

                fields(i,j,k,Jp_m) += +0.5_rt*kr*F;
                fields(i,j,k,Jm_m) += -0.5_rt*kr*F;
                fields(i,j,k,Jz_m) += -I*kz*F;
            }
        });
    }

    // Backward Fourier transform of J
    field_data.BackwardTransform( lev,
                                  *current[0], Idx.Jx,
                                  *current[1], Idx.Jy);
    field_data.BackwardTransform( lev,
                                  *current[2], Idx.Jz, 0 );
}

void
PsatdAlgorithmRZ::VayDeposition (const int /* lev */,
                                 SpectralFieldDataRZ& /*field_data*/,
                                 std::array<std::unique_ptr<amrex::MultiFab>,3>& /*current*/)
{
    amrex::Abort("Vay deposition not implemented in RZ geometry");
}
