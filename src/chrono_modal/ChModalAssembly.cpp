﻿// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Alessandro Tasora
// =============================================================================

#include "chrono_modal/ChModalAssembly.h"
#include "chrono/physics/ChSystem.h"
#include "chrono/fea/ChNodeFEAxyz.h"
#include "chrono/fea/ChNodeFEAxyzrot.h"

namespace chrono {

using namespace fea;
using namespace geometry;

namespace modal {

// Register into the object factory, to enable run-time dynamic creation and persistence
CH_FACTORY_REGISTER(ChModalAssembly)

ChModalAssembly::ChModalAssembly()
    : modal_variables(nullptr), n_modes_coords_w(0), is_modal(false), internal_nodes_update(true) {}

ChModalAssembly::ChModalAssembly(const ChModalAssembly& other) : ChAssembly(other) {
    is_modal = other.is_modal;
    modal_q = other.modal_q;
    modal_q_dt = other.modal_q_dt;
    modal_q_dtdt = other.modal_q_dtdt;
    custom_F_modal = other.custom_F_modal;
    internal_nodes_update = other.internal_nodes_update;
    m_custom_F_modal_callback = other.m_custom_F_modal_callback;
    m_custom_F_full_callback = other.m_custom_F_full_callback;

    //// TODO:  deep copy of the object lists (internal_bodylist, internal_linklist, internal_meshlist,
    /// internal_otherphysicslist)
}

ChModalAssembly::~ChModalAssembly() {
    RemoveAllInternalBodies();
    RemoveAllInternalLinks();
    RemoveAllInternalMeshes();
    RemoveAllInternalOtherPhysicsItems();
    if (modal_variables)
        delete modal_variables;
}

ChModalAssembly& ChModalAssembly::operator=(ChModalAssembly other) {
    ChModalAssembly tmp(other);
    swap(*this, other);
    return *this;
}

// Note: implement this as a friend function (instead of a member function swap(ChModalAssembly& other)) so that other
// classes that have a ChModalAssembly member (currently only ChSystem) could use it, the same way we use std::swap
// here.
void swap(ChModalAssembly& first, ChModalAssembly& second) {
    using std::swap;
    // swap(first.nbodies, second.nbodies);
    // ***TODO***
}

void ChModalAssembly::Clear() {
    ChAssembly::Clear();  // parent

    RemoveAllInternalBodies();
    RemoveAllInternalLinks();
    RemoveAllInternalMeshes();
    RemoveAllInternalOtherPhysicsItems();

    if (modal_variables)
        delete modal_variables;
}

// Assembly a sparse matrix by bordering square H with rectangular Cq.
//    HCQ = [ H  Cq' ]
//          [ Cq  0  ]
void util_sparse_assembly_2x2symm(
    Eigen::SparseMatrix<double, Eigen::ColMajor, int>& HCQ,  ///< resulting square sparse matrix (column major)
    const ChSparseMatrix& H,                                 ///< square sparse H matrix, n_v x n_v
    const ChSparseMatrix& Cq)                                ///< rectangular  sparse Cq  n_c x n_v
{
    int n_v = H.rows();
    int n_c = Cq.rows();
    HCQ.resize(n_v + n_c, n_v + n_c);
    HCQ.reserve(H.nonZeros() + 2 * Cq.nonZeros());
    HCQ.setZero();

    for (int k = 0; k < H.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(H, k); it; ++it) {
            HCQ.insert(it.row(), it.col()) = it.value();
        }

    for (int k = 0; k < Cq.outerSize(); ++k)
        for (ChSparseMatrix::InnerIterator it(Cq, k); it; ++it) {
            HCQ.insert(it.row() + n_v, it.col()) = it.value();  // insert Cq
            HCQ.insert(it.col(), it.row() + n_v) = it.value();  // insert Cq'
        }

    // This seems necessary in Release mode
    HCQ.makeCompressed();

    //***NOTE***
    // for some reason the HCQ matrix created via .insert() or .elementRef() or triplet insert, is
    // corrupt in Release mode, not in Debug mode. However, when doing a loop like the one below,
    // it repairs it.
    // ***TODO*** avoid this bad hack and find the cause of the release/debug difference.
    /*
    for (int k = 0; k < HCQ.rows(); ++k) {
        for (int j = 0; j < HCQ.cols(); ++j) {
            auto foo = HCQ.coeffRef(k, j);
            //GetLog() << HCQ.coeffRef(k,j) << " ";
        }
    }
    */
}

//---------------------------------------------------------------------------------------
void ChModalAssembly::SwitchModalReductionON(ChSparseMatrix& full_M,
                                             ChSparseMatrix& full_K,
                                             ChSparseMatrix& full_Cq,
                                             const ChModalSolveUndamped& n_modes_settings,
                                             const ChModalDamping& damping_model) {
    if (is_modal)
        return;

    this->SetupInitial();
    this->Setup();
    this->Update();

    // Steps of modal reduction

    // 1-
    //  to calculate the position of the mass center of the subsystem,
    //  then to determine the selection matrix S,
    //  then to determine the floating frame F as ChFrameMoving().
    //  note: both pos/vel of F are used in the modal method.

    // 2-
    //  to find a way to retrieve the pos/vel/acc of boundary and internal nodes: B, I
    //  then to determine the transformation matrices: P_B1, P_B2, P_I1, P_I2

    // 3-
    //  transform the full system matrices in the original mixed basis to be in the local frame of F,
    //  where P_B2, P_I2 will be used.
    //  the system matrices: full_M_loc, full_K_loc, full_R_loc, full_Cq_loc will be obtained.

    // 4-
    //  perform the modal reduction procedure in the local frame of F,
    //  the system matrices: M_red, K_red, R_red, Cq_red will be obtained.
    //  transformation matrices Psi, Psi_S, Psi_D will be obtained.
    //  todo: verify whether the equation K_IB*P_B1+K_II*P_I1==0 is true for the rigid-body mode shapes: \Phi_r = [P_B1;
    //  P_I1].
    //
    //

    // 2) fetch the initial state of assembly, full not reduced, as an initialization
    double fooT;
    full_assembly_x_old.setZero(this->ncoords, nullptr);
    full_assembly_v_old.setZero(this->ncoords_w, nullptr);
    this->IntStateGather(0, full_assembly_x_old, 0, full_assembly_v_old, fooT);

    // fetch the full state snapshot for this analysis
    modes_assembly_x0.setZero(this->ncoords, nullptr);  //[qB; qI]
    modes_assembly_x0 = full_assembly_x_old;

    this->ComputeMassCenter();

    this->UpdateFloatingFrameOfReference();

    // fetch the initial floating frame of reference F0 at the initial configuration
    this->floating_frame_F0 = this->floating_frame_F;

    this->ComputeLocalFullKRMmatrix();

    if (this->modal_reduction_type == Herting) {
        // 1) compute eigenvalue and eigenvectors
        int expected_eigs = 0;
        for (auto freq_span : n_modes_settings.freq_spans)
            expected_eigs += freq_span.nmodes;

        if (expected_eigs < 6) {
            GetLog() << "At least six rigid-body modes are required for the modal reduction. The default settings are "
                        "used.\n";
            this->ComputeModesExternalData(full_M_loc, full_K_loc, full_Cq_loc, ChModalSolveUndamped(6));
        } else
            this->ComputeModesExternalData(full_M_loc, full_K_loc, full_Cq_loc, n_modes_settings);

        // 3) bound ChVariables etc. to the modal coordinates, resize matrices, set as modal mode
        this->SetModalMode(true);
        this->SetupModalData(this->modes_V.cols());

        GetLog() << "*** Herting reduction is used.\n";
        for (int i = 0; i < this->modes_eig.size(); ++i)
            GetLog() << " Damped mode n." << i << "  frequency [Hz]: " << modes_freq(i) << "\n";

        this->UpdateTransformationMatrix();

        // 4) do the Herting reduction as in Sonneville, 2021
        DoModalReduction_Herting(damping_model);

    } else if (this->modal_reduction_type == Craig_Bampton) {
        ChSparseMatrix full_M_II_loc =
            full_M_loc.block(n_boundary_coords_w, n_boundary_coords_w, n_internal_coords_w, n_internal_coords_w);
        ChSparseMatrix full_K_II_loc =
            full_K_loc.block(n_boundary_coords_w, n_boundary_coords_w, n_internal_coords_w, n_internal_coords_w);
        ChSparseMatrix full_Cq_II_loc =
            full_Cq_loc.block(n_boundary_doc, n_boundary_coords_w, n_internal_doc, n_internal_coords_w);

        this->ComputeModesExternalData(full_M_II_loc, full_K_II_loc, full_Cq_II_loc, n_modes_settings);

        // std::vector<Eigen::Index> idxs;
        // for (Eigen::Index i = 0; i < this->modes_eig.size(); ++i)
        //     if (std::abs(this->modes_eig(i)) > 1e-5)
        //         idxs.push_back(i);
        //
        // ChMatrixDynamic<std::complex<double>> elastic_modes_V =
        //     modes_V(Eigen::all, idxs);
        // ChVectorDynamic<std::complex<double>> elastic_modes_eig = modes_eig(idxs);
        // ChVectorDynamic<double> elastic_modes_freq = modes_freq(idxs);
        // modes_V = elastic_modes_V;
        // modes_eig = elastic_modes_eig;
        // modes_freq = elastic_modes_freq;

        // 3) bound ChVariables etc. to the modal coordinates, resize matrices, set as modal mode
        this->SetModalMode(true);
        this->SetupModalData(this->modes_V.cols());

        GetLog() << "*** Craig Bampton reduction is used.\n";
        for (int i = 0; i < this->modes_eig.size(); ++i)
            GetLog() << " Damped mode n." << i << "  frequency [Hz]: " << modes_freq(i) << "\n";

        this->UpdateTransformationMatrix();

        // 4) do the Craig Bampton reduction as in Cardona, 2001
        DoModalReduction_CraigBamption(damping_model);

    } else {
        GetLog() << "The modal reduction type is specified incorrectly...\n";
        assert(0);
    }

    this->ComputeProjectionMatrix();

    // compute the modal K R M matrices
    ComputeInertialKRMmatrix();  // inertial M K R
    ComputeStiffnessMatrix();    // material stiffness and geometrical stiffness
    ComputeDampingMatrix();      // material damping
    ComputeModalKRMmatrix();

    // Debug dump data. ***TODO*** remove
    if (true) {
        ChStreamOutAsciiFile filePsi("dump_modal_Psi.dat");
        filePsi.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(Psi, filePsi);
        ChStreamOutAsciiFile fileM("dump_modal_M.dat");
        fileM.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_M, fileM);
        ChStreamOutAsciiFile fileK("dump_modal_K.dat");
        fileK.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_K, fileK);
        ChStreamOutAsciiFile fileR("dump_modal_R.dat");
        fileR.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_R, fileR);
        ChStreamOutAsciiFile fileCq("dump_modal_Cq.dat");
        fileCq.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->modal_Cq, fileCq);

        ChStreamOutAsciiFile fileM_red("dump_reduced_M.dat");
        fileM_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->M_red, fileM_red);
        ChStreamOutAsciiFile fileK_red("dump_reduced_K.dat");
        fileK_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->K_red, fileK_red);
        ChStreamOutAsciiFile fileR_red("dump_reduced_R.dat");
        fileR_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->R_red, fileR_red);
        ChStreamOutAsciiFile fileCq_red("dump_reduced_Cq.dat");
        fileCq_red.SetNumFormat("%.12g");
        StreamOutDenseMatlabFormat(this->Cq_red, fileCq_red);
    }
}

void ChModalAssembly::SwitchModalReductionON(const ChModalSolveUndamped& n_modes_settings,
                                             const ChModalDamping& damping_model) {
    if (is_modal)
        return;

    // 1) fetch the full (not reduced) mass and stiffness
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    // 2) compute modal reduction from full_M, full_K
    // this->SwitchModalReductionON_backup(full_M, full_K, full_Cq, n_modes_settings, damping_model);
    this->SwitchModalReductionON(full_M, full_K, full_Cq, n_modes_settings, damping_model);
}

void ChModalAssembly::ComputeMassCenter() {
    // Build a temporary mesh to collect all nodes and elements in the modal assembly because it happens
    // that the boundary nodes are added in the boundary 'meshlist' whereas their associated elements might
    // be in the 'internal_meshlist', leading to a mess in the mass computation.
    auto mmesh_bou_int = chrono_types::make_shared<ChMesh>();
    // collect boundary mesh
    for (auto& item : meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes())
                mmesh_bou_int->AddNode(node);
            for (auto& ele : mesh->GetElements())
                mmesh_bou_int->AddElement(ele);
        }
    }
    // collect internal mesh
    for (auto& item : internal_meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes())
                mmesh_bou_int->AddNode(node);
            for (auto& ele : mesh->GetElements())
                mmesh_bou_int->AddElement(ele);
        }
    }

    double mass_total = 0;
    ChVector<> mass_weighted_radius(0);

    // for boundary bodies
    for (auto& body : bodylist) {
        if (body->IsActive()) {
            mass_total += body->GetMass();
            mass_weighted_radius += body->GetMass() * body->GetPos();
        }
    }
    // for internal bodies
    for (auto& body : internal_bodylist) {
        if (body->IsActive()) {
            mass_total += body->GetMass();
            mass_weighted_radius += body->GetMass() * body->GetPos();
        }
    }

    // compute the mass properties of the mesh
    double mmesh_mass = 0;
    ChVector<> mmesh_com(0);
    ChMatrix33<> mmesh_inertia(0);
    mmesh_bou_int->ComputeMassProperties(mmesh_mass, mmesh_com, mmesh_inertia);
    mass_total += mmesh_mass;
    mass_weighted_radius += mmesh_mass * mmesh_com;

    if (mass_total) {
        this->com_x = mass_weighted_radius / mass_total;

        this->com_frame.SetPos(com_x);

        Eigen::EigenSolver<Eigen::MatrixXd> es(mmesh_inertia);
        ChMatrix33<> com_axis = es.eigenvectors().real();
        ChQuaternion q_axis = com_axis.Get_A_quaternion();

        this->com_frame.SetRot(q_axis);

    } else {
        // located at the position of the first boundary body/node of subassembly
        this->com_x = full_assembly_x_old.segment(0, 3);

        this->com_frame.SetPos(com_x);

        ChQuaternion q_axis = full_assembly_x_old.segment(3, 4);
        this->com_frame.SetRot(q_axis);
    }
}

void ChModalAssembly::DebugBlock() {
    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    ChStateDelta u_locred(bou_mod_coords_w, nullptr);
    ChStateDelta e_locred_def(bou_mod_coords_w, nullptr);
    ChStateDelta edt_locred_def(bou_mod_coords_w, nullptr);
    this->GetStateLocal(u_locred, e_locred_def, edt_locred_def, "definition");

    ChStateDelta e_locred_proj(bou_mod_coords_w, nullptr);
    ChStateDelta edt_locred_proj(bou_mod_coords_w, nullptr);
    this->GetStateLocal(u_locred, e_locred_proj, edt_locred_proj, "projection");

    GetLog() << "ChTime: " << ChTime << "\t";
    GetLog() << "floating_frame_F: " << floating_frame_F.GetPos().x() << "\t" << floating_frame_F.GetPos().y() << "\t"
             << floating_frame_F.GetPos().z() << "\t" << floating_frame_F.GetRot().Q_to_Rotv().x() << "\t"
             << floating_frame_F.GetRot().Q_to_Rotv().y() << "\t" << floating_frame_F.GetRot().Q_to_Rotv().z() << "\n";

    /***
    // displacement terms
    ***/

    // GetLog() << "u_locred: " << u_locred << "\n";

    ChVectorDynamic<> u_F(6);
    u_F = Q * u_locred;
    GetLog() << "u_F: " << u_F(0) << " " << u_F(1) << " " << u_F(2) << " " << u_F(3) << " " << u_F(4) << " " << u_F(5)
             << "\n";

    // check 1: six constraints to eliminate the redundant DOFs of floating frame F
    ChVectorDynamic<> C_F_locred(6);
    C_F_locred = U_locred.transpose() * M_red * e_locred_def;  // should be zero
    GetLog() << "C_F_locred: " << C_F_locred.norm() << "\t";

    // check 2: rigid body motion
    // ChVectorDynamic<> uF_rigid1(bou_mod_coords_w);
    // uF_rigid1 = U_locred * u_F;
    // ChVectorDynamic<> uF_rigid2(bou_mod_coords_w);
    // uF_rigid2 = P_parallel * u_locred;
    // ChVectorDynamic<> uF_rigid_cmp(bou_mod_coords_w);
    // uF_rigid_cmp = uF_rigid1 - uF_rigid2;  // should be zero
    // GetLog() << "uF_rigid_cmp: " << uF_rigid_cmp.norm() << "\t";

    // check 3: elastic deformation
    ChVectorDynamic<> e_locred_cmp;
    e_locred_cmp = e_locred_def - e_locred_proj;  // should be zero
    GetLog() << "e_locred_cmp: " << e_locred_cmp.norm() << "\n";

    /***
    // velocity terms
    ***/

    ChVectorDynamic<> udt_locred = P_W.transpose() * v_mod;
    ChVectorDynamic<> udt_F(6);
    udt_F = Q * udt_locred;
    // GetLog() << "udt_F: " << udt_F(0) << " " << udt_F(1) << " " << udt_F(2) << " " << udt_F(3) << " " << udt_F(4) <<
    // " " << udt_F(5) << "\t";

    ChVectorDynamic<> C_Fdt_locred(6);
    C_Fdt_locred = U_locred.transpose() * M_red * edt_locred_def;  // should be zero
    GetLog() << "C_Fdt_locred: " << C_Fdt_locred.norm() << "\t";

    // ChVectorDynamic<> udtF_rigid1(6);
    // udtF_rigid1 = U_locred * udt_F;
    // ChVectorDynamic<> udtF_rigid2(6);
    // udtF_rigid2 = P_parallel * udt_locred;
    // ChVectorDynamic<> udtF_rigid_cmp(6);
    // udtF_rigid_cmp = udtF_rigid1 - udtF_rigid2;  // should be zero
    // GetLog() << "udtF_rigid_cmp: " << udtF_rigid_cmp.norm() << "\t";

    ChVectorDynamic<> edt_locred_cmp;
    edt_locred_cmp = edt_locred_def - edt_locred_proj;  // should be zero
    GetLog() << "edt_locred_cmp: " << edt_locred_cmp.norm() << "\t";

    // ChVectorDynamic<> residual_F(bou_mod_coords_w);
    // residual_F.setZero();
    // residual_F -= P_W * P_perp.transpose() * (this->K_red * e_locred_proj) + this->modal_R * v_mod;
    // GetLog() << "residual_F: " << residual_F << "\t";

    double kinetic_energy = 0.5 * v_mod.transpose() * (M_sup * v_mod);
    double elastic_energy = 0.5 * e_locred_proj.transpose() * (K_red * e_locred_proj);
    double sum_energy = kinetic_energy + elastic_energy;
    GetLog() << "energy:  K=" << kinetic_energy << "\t V=" << elastic_energy << "\t Sum=" << sum_energy;

    GetLog() << "\n";
}

bool ChModalAssembly::UpdateFloatingFrameOfReference() {
    bool converged_flag_F = false;

    double fooT;
    ChState assembly_x;
    ChStateDelta assembly_v;
    assembly_x.setZero(this->ncoords, nullptr);
    assembly_v.setZero(this->ncoords_w, nullptr);
    this->IntStateGather(0, assembly_x, 0, assembly_v, fooT);

    // ChStateDelta assembly_a;
    // assembly_a.setZero(this->ncoords_w, nullptr);
    // this->IntStateGatherAcceleration(0, assembly_a);

    if (is_initialized_F == false) {
        floating_frame_F = com_frame;

        is_initialized_F = true;
        converged_flag_F = true;

        GetLog() << "*** Floating frame F is initialized at COG.\n";

    } else {
        // solve the configuration of the floating frame F using Newton-Raphson iteration

        // int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

        auto ComputeResidual_ConstrF = [&](ChVectorDynamic<>& mC_F) {
            this->UpdateTransformationMatrix();
            this->ComputeProjectionMatrix();

            ChStateDelta u_locred(bou_mod_coords_w, nullptr);
            ChStateDelta e_locred(bou_mod_coords_w, nullptr);
            ChStateDelta edt_locred(bou_mod_coords_w, nullptr);
            this->GetStateLocal(u_locred, e_locred, edt_locred, "definition");

            // six constraints on F to eliminate the redundant DOFs of floating frame F
            mC_F = this->U_locred.transpose() * (this->M_red * e_locred);  // expected to be zero
        };

        auto ComputeJacobian_ConstrF_Numerical = [&](ChMatrixDynamic<>& mJac_F) {
            ChVectorDynamic<> C_F0(6);
            ChVectorDynamic<> C_FD(6);
            ComputeResidual_ConstrF(C_F0);

            mJac_F.setZero(6, 6);
            ChVector<> pF0 = this->floating_frame_F.GetPos();
            ChQuaternion<> qF0 = this->floating_frame_F.GetRot();

            double delta_p = 1e-9;
            double delta_r = 1e-9;
            for (int i = 0; i < 3; ++i) {
                ChVector<> pFD = pF0;
                pFD[i] += delta_p;
                this->floating_frame_F.SetPos(pFD);
                ComputeResidual_ConstrF(C_FD);
                mJac_F.col(i) = (C_FD - C_F0) / delta_p;
                this->floating_frame_F.SetPos(pF0);
            }
            for (int i = 0; i < 3; ++i) {
                ChVector<> rotator(VNULL);
                rotator[i] = delta_r;
                ChQuaternion<> mdeltarotL;
                mdeltarotL.Q_from_Rotv(rotator);  // rot.in local basis - as in system wide vectors
                ChQuaternion<> qFD = qF0 * mdeltarotL;
                this->floating_frame_F.SetRot(qFD);
                ComputeResidual_ConstrF(C_FD);
                mJac_F.col(i + 3) = (C_FD - C_F0) / delta_r;
                this->floating_frame_F.SetRot(qF0);
            }
        };

        auto ComputeJacobian_ConstrF_Analytical = [&](ChMatrixDynamic<>& mJac_F) {
            mJac_F.setZero(6, 6);

            // ChStateDelta u_locred(bou_mod_coords_w, nullptr);
            // ChStateDelta e_locred(bou_mod_coords_w, nullptr);
            // ChStateDelta edt_locred(bou_mod_coords_w, nullptr);
            // this->GetStateLocal(u_locred, e_locred, edt_locred, "definition");

            // ChVectorDynamic<> h_loc_zeta(n_boundary_coords_w + n_modes_coords_w);
            // h_loc_zeta = M_red * e_locred;

            // ChMatrix33<> mXi_F1;
            // mXi_F1.setZero();
            // ChMatrix33<> mXi_F2;
            // mXi_F2.setZero();
            // ChMatrix33<> mXi_F3;
            // mXi_F3.setZero();

            // ChMatrixDynamic<> mXi_F;
            // mXi_F.setZero(6, 6);
            //// ChMatrixDynamic<> mXi_Hext;
            //// mXi_Hext.setZero(6, n_boundary_coords_w + n_modes_coords_w);

            // for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
            //     ChVector<> F_loc = h_loc_zeta.segment(6 * i_bou, 3);
            //     ChVector<> M_loc = h_loc_zeta.segment(6 * i_bou + 3, 3);
            //     ChVector<> r_B = assembly_x.segment(7 * i_bou, 3);
            //     ChQuaternion<> quat_bou = assembly_x.segment(7 * i_bou + 3, 4);
            //     ChMatrix33<> R_B(quat_bou);
            //     mXi_F1 += floating_frame_F.GetA() * ChStarMatrix33<>(F_loc);
            //     mXi_F3 += ChStarMatrix33<>(F_loc) *
            //                   (floating_frame_F.GetA().transpose() * ChStarMatrix33<>(r_B -
            //                   floating_frame_F.GetPos()) *
            //                    floating_frame_F.GetA()) -
            //               ChStarMatrix33<>(floating_frame_F.GetA().transpose() * (R_B * M_loc));
            //     // mXi_Hext.block(3, 6 * i_bou, 3, 3) = ChStarMatrix33<>(F_loc) *
            //     floating_frame_F.GetA().transpose();
            //     // mXi_Hext.block(3, 6 * i_bou + 3, 3, 3) =
            //     // floating_frame_F.GetA().transpose() * (R_B * ChStarMatrix33<>(M_loc));
            // }
            // mXi_F2 = mXi_F1.transpose();
            // mXi_F << ChMatrix33<>(0), mXi_F1, mXi_F2, mXi_F3;

            //ChMatrixDynamic<> UTMU = U_locred.transpose() * M_red * U_locred;
            // mJac_F = -mXi_F - UTMU;

            mJac_F = -U_locred.transpose() * M_red * U_locred;
        };

        int iteration_count = 0;
        double tol = 1e-16;

        while (converged_flag_F == false && iteration_count < 6) {
            ChVectorDynamic<> C_F0(6);
            ComputeResidual_ConstrF(C_F0);

            // Jacobian of six constraints w.r.t. floating frame F
            // ChMatrixDynamic<> Jac_F_num;
            // ComputeJacobian_ConstrF_Numerical(Jac_F_num);
            ChMatrixDynamic<> Jac_F_ana;
            ComputeJacobian_ConstrF_Analytical(Jac_F_ana);
            // GetLog() << "\n (Jac_F_num - Jac_F_ana).norm():\t" << (Jac_F_num - Jac_F_ana).norm() / Jac_F_ana.norm();

            ChVectorDynamic<> delta_F(6);
            delta_F = Jac_F_ana.colPivHouseholderQr().solve(-C_F0);
            ChVector<> pos_F = floating_frame_F.GetPos() + delta_F.head(3);

            ChQuaternion<> incr_rotF(QNULL);
            incr_rotF.Q_from_Rotv(ChVector<>(delta_F.tail(3)));  // rot.in local basis - as in system wide vectors
            ChQuaternion<> rot_F = floating_frame_F.GetRot() * incr_rotF;

            floating_frame_F.SetPos(pos_F);
            floating_frame_F.SetRot(rot_F);

            if (C_F0.norm() < tol)  // tend to be more stable
            // if (delta_F.norm() < tol)//tend to be more unstable
            {
                converged_flag_F = true;
            }

            iteration_count++;
        }

        this->UpdateTransformationMatrix();

        this->ComputeProjectionMatrix();

        ChVectorDynamic<> vel_F(6);  // qdt_F
        vel_F = Q * (P_W.transpose() * assembly_v);
        floating_frame_F.SetPos_dt(vel_F.head(3));
        floating_frame_F.SetWvel_loc(vel_F.tail(3));
    }

    // floating_frame_F_old = floating_frame_F;

    if (verbose) {
        GetLog() << "\n***Debug in UpdateFloatingFrameOfReference():\n";
        DebugBlock();
    }

    return converged_flag_F;
}

void ChModalAssembly::UpdateTransformationMatrix() {
    // update P_B1, P_B2, P_I1, P_I2, P_W, U_locred

    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    // fetch the state snapshot (modal reduced)
    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    //  for boudnary bodies and nodes
    P_B1.setZero(n_boundary_coords_w, 6);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_B1.block(6 * i_bou, 0, 3, 3) = ChMatrix33<>(1.0);
        P_B1.block(6 * i_bou, 3, 3, 3) =
            -ChStarMatrix33<>(ChVector<>(x_mod.segment(7 * i_bou, 3)) - floating_frame_F.GetPos()) *
            floating_frame_F.GetA();
        // todo:boundary nodes must have 4 rotational DOFs from quaternion parametrization
        ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
        P_B1.block(6 * i_bou + 3, 3, 3, 3) = ChMatrix33<>(quat_bou.GetConjugate() * floating_frame_F.GetRot());
    }

    P_B2.setIdentity(n_boundary_coords_w, n_boundary_coords_w);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_B2.block(6 * i_bou, 6 * i_bou, 3, 3) = floating_frame_F.GetA();
    }

    // for internal bodies and nodes
    if (internal_nodes_update) {
        P_I1.setZero(n_internal_coords_w, 6);
        for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
            P_I1.block(6 * i_int, 0, 3, 3) = ChMatrix33<>(1.0);
            P_I1.block(6 * i_int, 3, 3, 3) =
                -ChStarMatrix33<>(ChVector<>(full_assembly_x_old.segment(n_boundary_coords + 7 * i_int, 3)) -
                                  floating_frame_F.GetPos()) *
                floating_frame_F.GetA();
            // todo:internal nodes must have 4 rotational DOFs from quaternion parametrization
            ChQuaternion<> quat_int = full_assembly_x_old.segment(n_boundary_coords + 7 * i_int + 3, 4);
            P_I1.block(6 * i_int + 3, 3, 3, 3) = ChMatrix33<>(quat_int.GetConjugate() * floating_frame_F.GetRot());
        }

        P_I2.setIdentity(n_internal_coords_w, n_internal_coords_w);
        for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
            P_I2.block(6 * i_int, 6 * i_int, 3, 3) = floating_frame_F.GetA();
        }
    }

    P_W.setIdentity(bou_mod_coords_w, bou_mod_coords_w);
    P_W.topLeftCorner(n_boundary_coords_w, n_boundary_coords_w) = P_B2;
}

void ChModalAssembly::ComputeProjectionMatrix() {
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    U_locred.setZero(bou_mod_coords_w, 6);
    U_locred.topRows(n_boundary_coords_w) = P_B2.transpose() * P_B1;

    ChMatrixDynamic<> UTMU = U_locred.transpose() * M_red * U_locred;  // of size (6,6)
    UTMU_inv_solver = UTMU.colPivHouseholderQr();
    Q.setZero(6, bou_mod_coords_w);
    Q = UTMU_inv_solver.solve(U_locred.transpose() * M_red);

    P_parallel.setZero(bou_mod_coords_w, bou_mod_coords_w);
    P_parallel = U_locred * Q;

    ChMatrixDynamic<> I_bm;
    I_bm.setIdentity(bou_mod_coords_w, bou_mod_coords_w);
    P_perp.setZero(bou_mod_coords_w, bou_mod_coords_w);
    P_perp = I_bm - P_parallel;
}

void ChModalAssembly::ComputeLocalFullKRMmatrix() {
    // 1) fetch the full (not reduced) mass and stiffness
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_R;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyDampingMatrix(&full_R);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    // todo: to fill the sparse P_BI in a more straightforward and efficient way
    ChMatrixDynamic<> P_BI;
    P_BI.setIdentity(n_boundary_coords_w + n_internal_coords_w, n_boundary_coords_w + n_internal_coords_w);
    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        P_BI.block(6 * i_bou, 6 * i_bou, 3, 3) = floating_frame_F.GetA();
    }
    for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
        P_BI.block(n_boundary_coords_w + 6 * i_int, n_boundary_coords_w + 6 * i_int, 3, 3) = floating_frame_F.GetA();
    }
    ChSparseMatrix P_BI_sp = P_BI.sparseView();

    full_M_loc = P_BI_sp.transpose() * full_M * P_BI_sp;
    full_K_loc = P_BI_sp.transpose() * full_K * P_BI_sp;
    full_R_loc = P_BI_sp.transpose() * full_R * P_BI_sp;
    full_Cq_loc = full_Cq * P_BI_sp;

    full_M_loc.makeCompressed();
    full_K_loc.makeCompressed();
    full_R_loc.makeCompressed();
    full_Cq_loc.makeCompressed();
}

void ChModalAssembly::DoModalReduction_Herting(const ChModalDamping& damping_model) {
    // 1) compute eigenvalue and eigenvectors of the full subsystem.
    // It is calculated in the local floating frame of reference F, thus there must be six rigid-body modes.
    // It is expected that the eigenvalues of the six rigid-body modes are zero, but
    // maybe nonzero if the geometrical stiffness matrix Kg is involved, we also have the opportunity
    // to consider the inertial damping and inertial stiffness matrices Ri,Ki respectively.

    assert(this->modes_V.cols() >= 6);  // at least six rigid-body modes are required.

    // K_IIc = [  K_II   Cq_II' ]
    //         [ Cq_II     0    ]
    ChSparseMatrix K_II_loc = full_K_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                               this->n_internal_coords_w, this->n_internal_coords_w);

    Eigen::SparseMatrix<double> K_IIc_loc;
    if (this->n_internal_doc_w) {
        ChSparseMatrix Cq_II_loc = full_Cq_loc.block(this->n_boundary_doc_w, this->n_boundary_coords_w,
                                                     this->n_internal_doc_w, this->n_internal_coords_w);
        util_sparse_assembly_2x2symm(K_IIc_loc, K_II_loc, Cq_II_loc);
    } else
        K_IIc_loc = K_II_loc;
    K_IIc_loc.makeCompressed();

    // Matrix of static modes (constrained, so use K_IIc instead of K_II,
    // the original unconstrained Herting reduction is Psi_S = - K_II^{-1} * K_IB
    //
    // Psi_S_C = {Psi_S; Psi_S_LambdaI} = - K_IIc^{-1} * {K_IB ; Cq_IB}
    ChSparseMatrix Cq_IB_loc =
        full_Cq_loc.block(this->n_boundary_doc_w, 0, this->n_internal_doc_w, this->n_boundary_coords_w);
    Psi_S.setZero(this->n_internal_coords_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_LambdaI(this->n_internal_doc_w, this->n_boundary_coords_w);

    // avoid computing K_IIc^{-1}, effectively do n times a linear solve:
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.analyzePattern(K_IIc_loc);
    solver.factorize(K_IIc_loc);
    ChSparseMatrix K_IB_loc =
        full_K_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);
    for (int i = 0; i < this->n_boundary_coords_w; ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
        if (this->n_internal_doc_w)
            rhs << K_IB_loc.col(i).toDense(), Cq_IB_loc.col(i).toDense();
        else
            rhs << K_IB_loc.col(i).toDense();

        ChVectorDynamic<> x = solver.solve(rhs.sparseView());

        Psi_S.col(i) = -x.head(this->n_internal_coords_w);
        Psi_S_C.col(i) = -x;
        if (this->n_internal_doc_w)
            Psi_S_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
    }

    ChVectorDynamic<> c_modes(this->modes_V.cols());
    c_modes.setOnes();

    for (int i_try = 0; i_try < 2; i_try++) {
        // The modal shapes of the first six rigid-body modes solved from the eigensolver might be not accurate,
        // leanding to potential numerical instability. Thus, we construct the rigid-body modal shapes directly.
        this->modes_V.block(0, 0, this->n_boundary_coords_w, 6) = P_B2.transpose() * P_B1;
        this->modes_V.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, 6) = P_I2.transpose() * P_I1;

        for (int i_mode = 0; i_mode < this->modes_V.cols(); ++i_mode) {
            if (c_modes(i_mode))
                // Normalize modes_V to improve the condition of M_red.
                // When i_try==0, c_modes==1, it doesnot change modes_V, but tries to obtain M_red and then find the
                // suitable coefficents c_modes;
                // When i_try==1, c_modes works to improve the condition of M_red for the sake of numerical stability.
                this->modes_V.col(i_mode) *= c_modes(i_mode);
            else
                this->modes_V.col(i_mode).normalize();
        }

        // ChMatrixDynamic<> check_orthogonality = modes_V.real().transpose() * full_M_loc * modes_V.real();
        // GetLog() << "check_orthogonality:\n" << check_orthogonality << "\n";

        ChMatrixDynamic<> V_B = this->modes_V.block(0, 0, this->n_boundary_coords_w, this->n_modes_coords_w).real();
        ChMatrixDynamic<> V_I =
            this->modes_V.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_modes_coords_w).real();

        // Matrix of dynamic modes (V_B and V_I already computed as constrained eigenmodes,
        // but use K_IIc instead of K_II anyway, to reuse K_IIc already factored before)
        //
        // Psi_D_C = {Psi_D; Psi_D_LambdaI} = - K_IIc^{-1} * {(M_IB * V_B + M_II * V_I) ; 0}
        Psi_D.setZero(this->n_internal_coords_w, this->n_modes_coords_w);
        ChMatrixDynamic<> Psi_D_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_modes_coords_w);
        ChMatrixDynamic<> Psi_D_LambdaI(this->n_internal_doc_w, this->n_modes_coords_w);

        ChSparseMatrix M_II_loc = full_M_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                                   this->n_internal_coords_w, this->n_internal_coords_w);
        ChSparseMatrix M_IB_loc =
            full_M_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);
        ChMatrixDynamic<> rhs_top = M_IB_loc * V_B + M_II_loc * V_I;
        for (int i = 0; i < this->n_modes_coords_w; ++i) {
            ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
            if (this->n_internal_doc_w)
                rhs << rhs_top.col(i), Eigen::VectorXd::Zero(this->n_internal_doc_w);
            else
                rhs << rhs_top.col(i);

            ChVectorDynamic<> x = solver.solve(rhs.sparseView());

            Psi_D.col(i) = -x.head(this->n_internal_coords_w);
            Psi_D_C.col(i) = -x;
            if (this->n_internal_doc_w)
                Psi_D_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
        }

        // Psi = [ I     0    ]
        //       [Psi_S  Psi_D]
        Psi.setZero(this->n_boundary_coords_w + this->n_internal_coords_w,
                    this->n_boundary_coords_w + this->n_modes_coords_w);
        //***TODO*** maybe prefer sparse Psi matrix, especially for upper blocks...

        Psi << Eigen::MatrixXd::Identity(n_boundary_coords_w, n_boundary_coords_w),
            Eigen::MatrixXd::Zero(n_boundary_coords_w, n_modes_coords_w), Psi_S, Psi_D;

        // Modal reduction of the M K matrices.
        // The tangent mass and stiffness matrices consists of:
        // Linear mass matrix
        // Linear material stiffness matrix, geometric stiffness matrix, inertial stiffness matrix
        // Linear structural damping matrix, inertial damping matrix (gyroscopic matrix, might affect the numerical
        // stability)
        this->M_red = Psi.transpose() * full_M_loc * Psi;
        this->K_red = Psi.transpose() * full_K_loc * Psi;
        this->K_red.block(0, n_boundary_coords_w, n_boundary_coords_w, n_modes_coords_w).setZero();
        this->K_red.block(n_boundary_coords_w, 0, n_modes_coords_w, n_boundary_coords_w).setZero();

        // Maybe also have a reduced Cq matrix......
        ChSparseMatrix Cq_B_loc = full_Cq_loc.topRows(this->n_boundary_doc_w);
        this->Cq_red = Cq_B_loc * Psi;

        // Initialize the reduced damping matrix
        this->R_red.setZero(this->M_red.rows(), this->M_red.cols());  // default R=0 , zero damping

        // Find the suitable coefficients 'c_modes' to normalize 'modes_V' to improve the condition number of 'M_red'.
        if (i_try < 1) {
            double expected_mass = this->M_red.diagonal().head(n_boundary_coords_w).mean();
            for (int i_mode = 0; i_mode < this->modes_V.cols(); ++i_mode)
                c_modes(i_mode) =
                    pow(expected_mass / this->M_red(n_boundary_coords_w + i_mode, n_boundary_coords_w + i_mode), 0.5);
        }
    }

    // Reset to zero all the atomic masses of the boundary nodes because now their mass is represented by
    // this->modal_M NOTE! this should be made more generic and future-proof by implementing a virtual method ex.
    // RemoveMass() in all ChPhysicsItem
    for (auto& body : bodylist) {
        body->SetMass(0);
        body->SetInertia(VNULL);
    }
    for (auto& item : this->meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node))
                    xyz->SetMass(0);
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    xyzrot->SetMass(0);
                    xyzrot->GetInertia().setZero();
                }
            }
        }
    }

    // Modal reduction of R damping matrix: compute using user-provided damping model.
    // todo: maybe the Cq_red is necessary for specifying the suitable modal damping ratios.
    // ChModalDampingNone damping_model;
    damping_model.ComputeR(*this, this->M_red, this->K_red, Psi, this->R_red);
    // R_red.setZero();  // todo:set zero for test temporarily

    // Invalidate results of the initial eigenvalue analysis because now the DOFs are different after reduction,
    // to avoid that one could be tempted to plot those eigenmodes, which now are not exactly the ones of the
    // reduced assembly.
    this->modes_damping_ratio.resize(0);
    this->modes_eig.resize(0);
    this->modes_freq.resize(0);
    this->modes_V.resize(0, 0);
}

void ChModalAssembly::DoModalReduction_CraigBamption(const ChModalDamping& damping_model) {
    // 1) compute eigenvalue and eigenvectors of the full subsystem.
    // It is calculated in the local floating frame of reference F, thus there must be six rigid-body modes.
    // It is expected that the eigenvalues of the six rigid-body modes are zero, but
    // maybe nonzero if the geometrical stiffness matrix Kg is involved, we also have the opportunity
    // to consider the inertial damping and inertial stiffness matrices Ri,Ki respectively.

    // K_IIc = [  K_II   Cq_II' ]
    //         [ Cq_II     0    ]
    ChSparseMatrix K_II_loc = full_K_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                               this->n_internal_coords_w, this->n_internal_coords_w);

    Eigen::SparseMatrix<double> K_IIc_loc;
    if (this->n_internal_doc_w) {
        ChSparseMatrix Cq_II_loc = full_Cq_loc.block(this->n_boundary_doc_w, this->n_boundary_coords_w,
                                                     this->n_internal_doc_w, this->n_internal_coords_w);
        util_sparse_assembly_2x2symm(K_IIc_loc, K_II_loc, Cq_II_loc);
    } else
        K_IIc_loc = K_II_loc;
    K_IIc_loc.makeCompressed();

    // Matrix of static modes (constrained, so use K_IIc instead of K_II,
    // the original unconstrained Herting reduction is Psi_S = - K_II^{-1} * K_IB
    //
    // Psi_S_C = {Psi_S; Psi_S_LambdaI} = - K_IIc^{-1} * {K_IB ; Cq_IB}
    ChSparseMatrix Cq_IB_loc =
        full_Cq_loc.block(this->n_boundary_doc_w, 0, this->n_internal_doc_w, this->n_boundary_coords_w);
    Psi_S.setZero(this->n_internal_coords_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_boundary_coords_w);
    ChMatrixDynamic<> Psi_S_LambdaI(this->n_internal_doc_w, this->n_boundary_coords_w);

    // avoid computing K_IIc^{-1}, effectively do n times a linear solve:
    Eigen::SparseQR<Eigen::SparseMatrix<double>, Eigen::COLAMDOrdering<int>> solver;
    solver.analyzePattern(K_IIc_loc);
    solver.factorize(K_IIc_loc);
    ChSparseMatrix K_IB_loc =
        full_K_loc.block(this->n_boundary_coords_w, 0, this->n_internal_coords_w, this->n_boundary_coords_w);
    for (int i = 0; i < this->n_boundary_coords_w; ++i) {
        ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
        if (this->n_internal_doc_w)
            rhs << K_IB_loc.col(i).toDense(), Cq_IB_loc.col(i).toDense();
        else
            rhs << K_IB_loc.col(i).toDense();

        ChVectorDynamic<> x = solver.solve(rhs.sparseView());

        Psi_S.col(i) = -x.head(this->n_internal_coords_w);
        Psi_S_C.col(i) = -x;
        if (this->n_internal_doc_w)
            Psi_S_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
    }

    ChVectorDynamic<> c_modes(this->modes_V.cols());
    c_modes.setOnes();

    for (int i_try = 0; i_try < 2; i_try++) {
        for (int i_mode = 0; i_mode < this->modes_V.cols(); ++i_mode) {
            if (c_modes(i_mode))
                // Normalize modes_V to improve the condition of M_red.
                // When i_try==0, c_modes==1, it doesnot change modes_V, but tries to obtain M_red and then find the
                // suitable coefficents c_modes;
                // When i_try==1, c_modes works to improve the condition of M_red for the sake of numerical stability.
                this->modes_V.col(i_mode) *= c_modes(i_mode);
            else
                this->modes_V.col(i_mode).normalize();
        }

        // ChMatrixDynamic<> check_orthogonality = modes_V.real().transpose() * full_M_loc * modes_V.real();
        // GetLog() << "check_orthogonality:\n" << check_orthogonality << "\n";

        ChMatrixDynamic<> V_I = this->modes_V.block(0, 0, this->n_internal_coords_w, this->n_modes_coords_w).real();

        // Matrix of dynamic modes (V_I already computed as constrained eigenmodes,
        // but use K_IIc instead of K_II anyway, to reuse K_IIc already factored before)
        //
        // Psi_D_C = {Psi_D; Psi_D_LambdaI} = - K_IIc^{-1} * {(M_II * V_I) ; 0}
        Psi_D.setZero(this->n_internal_coords_w, this->n_modes_coords_w);
        ChMatrixDynamic<> Psi_D_C(this->n_internal_coords_w + this->n_internal_doc_w, this->n_modes_coords_w);
        ChMatrixDynamic<> Psi_D_LambdaI(this->n_internal_doc_w, this->n_modes_coords_w);

        ChSparseMatrix M_II_loc = full_M_loc.block(this->n_boundary_coords_w, this->n_boundary_coords_w,
                                                   this->n_internal_coords_w, this->n_internal_coords_w);
        ChMatrixDynamic<> rhs_top = M_II_loc * V_I;
        for (int i = 0; i < this->n_modes_coords_w; ++i) {
            ChVectorDynamic<> rhs(this->n_internal_coords_w + this->n_internal_doc_w);
            if (this->n_internal_doc_w)
                rhs << rhs_top.col(i), Eigen::VectorXd::Zero(this->n_internal_doc_w);
            else
                rhs << rhs_top.col(i);

            ChVectorDynamic<> x = solver.solve(rhs.sparseView());

            Psi_D.col(i) = -x.head(this->n_internal_coords_w);
            Psi_D_C.col(i) = -x;
            if (this->n_internal_doc_w)
                Psi_D_LambdaI.col(i) = -x.tail(this->n_internal_doc_w);
        }

        // Psi = [ I     0    ]
        //       [Psi_S  Psi_D]
        Psi.setZero(this->n_boundary_coords_w + this->n_internal_coords_w,
                    this->n_boundary_coords_w + this->n_modes_coords_w);
        //***TODO*** maybe prefer sparse Psi matrix, especially for upper blocks...

        Psi << Eigen::MatrixXd::Identity(n_boundary_coords_w, n_boundary_coords_w),
            Eigen::MatrixXd::Zero(n_boundary_coords_w, n_modes_coords_w), Psi_S, Psi_D;

        // Modal reduction of the M K matrices.
        // The tangent mass and stiffness matrices consists of:
        // Linear mass matrix
        // Linear material stiffness matrix, geometric stiffness matrix, inertial stiffness matrix
        // Linear structural damping matrix, inertial damping matrix (gyroscopic matrix, might affect the numerical
        // stability)
        this->M_red = Psi.transpose() * full_M_loc * Psi;
        this->K_red = Psi.transpose() * full_K_loc * Psi;
        this->K_red.block(0, n_boundary_coords_w, n_boundary_coords_w, n_modes_coords_w).setZero();
        this->K_red.block(n_boundary_coords_w, 0, n_modes_coords_w, n_boundary_coords_w).setZero();

        // Maybe also have a reduced Cq matrix......
        ChSparseMatrix Cq_B_loc = full_Cq_loc.topRows(this->n_boundary_doc_w);
        this->Cq_red = Cq_B_loc * Psi;

        // Initialize the reduced damping matrix
        this->R_red.setZero(this->M_red.rows(), this->M_red.cols());  // default R=0 , zero damping

        // Find the suitable coefficients 'c_modes' to normalize 'modes_V' to improve the condition number of 'M_red'.
        if (i_try < 1) {
            double expected_mass = this->M_red.diagonal().head(n_boundary_coords_w).mean();
            for (int i_mode = 0; i_mode < this->modes_V.cols(); ++i_mode)
                c_modes(i_mode) =
                    pow(expected_mass / this->M_red(n_boundary_coords_w + i_mode, n_boundary_coords_w + i_mode), 0.5);
        }
    }

    // Reset to zero all the atomic masses of the boundary nodes because now their mass is represented by
    // this->modal_M NOTE! this should be made more generic and future-proof by implementing a virtual method ex.
    // RemoveMass() in all ChPhysicsItem
    for (auto& body : bodylist) {
        body->SetMass(0);
        body->SetInertia(VNULL);
    }
    for (auto& item : this->meshlist) {
        if (auto mesh = std::dynamic_pointer_cast<ChMesh>(item)) {
            for (auto& node : mesh->GetNodes()) {
                if (auto xyz = std::dynamic_pointer_cast<ChNodeFEAxyz>(node))
                    xyz->SetMass(0);
                if (auto xyzrot = std::dynamic_pointer_cast<ChNodeFEAxyzrot>(node)) {
                    xyzrot->SetMass(0);
                    xyzrot->GetInertia().setZero();
                }
            }
        }
    }

    // Modal reduction of R damping matrix: compute using user-provided damping model.
    // todo: maybe the Cq_red is necessary for specifying the suitable modal damping ratios.
    // ChModalDampingNone damping_model;
    damping_model.ComputeR(*this, this->M_red, this->K_red, Psi, this->R_red);
    // R_red.setZero();  // todo:set zero for test temporarily

    // Invalidate results of the initial eigenvalue analysis because now the DOFs are different after reduction,
    // to avoid that one could be tempted to plot those eigenmodes, which now are not exactly the ones of the
    // reduced assembly.
    this->modes_damping_ratio.resize(0);
    this->modes_eig.resize(0);
    this->modes_freq.resize(0);
    this->modes_V.resize(0, 0);
}

void ChModalAssembly::GetXi_Uloc_VD(const ChVectorDynamic<>& mv_loc, ChMatrixDynamic<>& mXi) {
    assert(mv_loc.rows() == 6);

    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    ChMatrixDynamic<> mXi_Vext;
    mXi_Vext.setZero(this->n_boundary_coords_w + this->n_modes_coords_w, 6);

    ChMatrixDynamic<> mXi_Dext;
    mXi_Dext.setZero(this->n_boundary_coords_w + this->n_modes_coords_w,
                     this->n_boundary_coords_w + this->n_modes_coords_w);

    ChVector<> F_loc = mv_loc.segment(0, 3);
    ChVector<> M_loc = mv_loc.segment(3, 3);

    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
        ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
        ChMatrix33<> R_B(quat_bou);
        mXi_Vext.block(6 * i_bou, 0, 3, 3) = -ChStarMatrix33<>(M_loc) * floating_frame_F.GetA().transpose();
        mXi_Vext.block(6 * i_bou, 3, 3, 3) =
            ChStarMatrix33<>(floating_frame_F.GetA().transpose() * F_loc) +
            ChStarMatrix33<>(M_loc) * (floating_frame_F.GetA().transpose() *
                                       ChStarMatrix33<>(r_B - floating_frame_F.GetPos()) * floating_frame_F.GetA());
        mXi_Vext.block(6 * i_bou + 3, 3, 3, 3) = -R_B.transpose() * floating_frame_F.GetA() * ChStarMatrix33<>(M_loc);

        mXi_Dext.block(6 * i_bou, 6 * i_bou, 3, 3) = ChStarMatrix33<>(M_loc) * floating_frame_F.GetA().transpose();
        mXi_Dext.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) =
            ChStarMatrix33<>(R_B.transpose() * (floating_frame_F.GetA() * M_loc));
    }

    mXi.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    mXi = mXi_Vext * Q * P_W.transpose() + mXi_Dext;
}

void ChModalAssembly::GetXi_UlocT_FH(const ChVectorDynamic<>& mv_loc, ChMatrixDynamic<>& mXi) {
    assert(mv_loc.rows() == this->n_boundary_coords_w + this->n_modes_coords_w);

    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    ChMatrix33<> mXi_F1;
    mXi_F1.setZero();
    ChMatrix33<> mXi_F2;
    mXi_F2.setZero();
    ChMatrix33<> mXi_F3;
    mXi_F3.setZero();

    ChMatrixDynamic<> mXi_F;
    mXi_F.setZero(6, 6);
    ChMatrixDynamic<> mXi_Hext;
    mXi_Hext.setZero(6, n_boundary_coords_w + n_modes_coords_w);

    for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
        ChVector<> F_loc = mv_loc.segment(6 * i_bou, 3);
        ChVector<> M_loc = mv_loc.segment(6 * i_bou + 3, 3);
        ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
        ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
        ChMatrix33<> R_B(quat_bou);
        mXi_F1 += floating_frame_F.GetA() * ChStarMatrix33<>(F_loc);
        mXi_F3 +=
            ChStarMatrix33<>(F_loc) * (floating_frame_F.GetA().transpose() *
                                       ChStarMatrix33<>(r_B - floating_frame_F.GetPos()) * floating_frame_F.GetA()) -
            ChStarMatrix33<>(floating_frame_F.GetA().transpose() * (R_B * M_loc));
        mXi_Hext.block(3, 6 * i_bou, 3, 3) = ChStarMatrix33<>(F_loc) * floating_frame_F.GetA().transpose();
        mXi_Hext.block(3, 6 * i_bou + 3, 3, 3) = floating_frame_F.GetA().transpose() * (R_B * ChStarMatrix33<>(M_loc));
    }
    mXi_F2 = mXi_F1.transpose();
    mXi_F << ChMatrix33<>(0), mXi_F1, mXi_F2, mXi_F3;

    mXi.setZero(6, n_boundary_coords_w + n_modes_coords_w);
    mXi = mXi_F * Q * P_W.transpose() + mXi_Hext;
}

void ChModalAssembly::ComputeInertialKRMmatrix() {
    if (!is_modal)
        return;

    this->M_sup.setZero();
    this->Ri_sup.setZero();
    this->Ki_sup.setZero();

    // inertial mass matrix
    M_sup = P_W * M_red * P_W.transpose();

    if (use_inertial_damping || use_inertial_stiffness) {
        // fetch the state snapshot (modal reduced)
        int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;
        double fooT;
        ChState x_mod;       // =[qB; eta]
        ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        x_mod.setZero(bou_mod_coords, nullptr);
        v_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        ChStateDelta a_mod;  // =[qB_dtdt; eta_dtdt]
        a_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGatherAcceleration(0, a_mod);

        ChMatrixDynamic<> V;
        ChMatrixDynamic<> O_B;
        ChMatrixDynamic<> O_F;
        V.setZero(bou_mod_coords_w, 6);
        O_B.setZero(bou_mod_coords_w, bou_mod_coords_w);
        O_F.setZero(bou_mod_coords_w, bou_mod_coords_w);

        for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
            V.block(6 * i_bou, 3, 3, 3) =
                ChStarMatrix33<>(floating_frame_F.GetRot().RotateBack(v_mod.segment(6 * i_bou, 3)));
            O_B.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) = ChStarMatrix33<>(v_mod.segment(6 * i_bou + 3, 3));
            O_F.block(6 * i_bou, 6 * i_bou, 3, 3) = ChStarMatrix33<>(floating_frame_F.GetWvel_loc());
        }

        //// quadratic velocity term, not used here
        // ChMatrixDynamic<> mat_O = P_W * (O_F + O_B) * M_red * P_W.transpose();
        // ChMatrixDynamic<> mat_M = P_W * M_red * V * Q * P_W.transpose();
        // g_quad.setZero();
        // g_quad = (mat_O + mat_M - mat_M.transpose()) * v_mod;

        ChVectorDynamic<> f_loc_C =
            M_red * (P_W.transpose() * a_mod) +
            ((O_F + O_B) * M_red + M_red * V * Q - (M_red * V * Q).transpose()) * (P_W.transpose() * v_mod);

        ChMatrixDynamic<> V_iner;
        ChMatrixDynamic<> V_acc;
        ChMatrixDynamic<> V_rmom;
        ChMatrixDynamic<> O_thetamom;
        V_iner.setZero(bou_mod_coords_w, 6);
        V_acc.setZero(bou_mod_coords_w, 6);
        V_rmom.setZero(bou_mod_coords_w, 6);
        O_thetamom.setZero(bou_mod_coords_w, bou_mod_coords_w);
        ChVectorDynamic<> momen = M_red * (P_W.transpose() * v_mod);
        for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
            V_iner.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(f_loc_C.segment(6 * i_bou, 3));
            V_acc.block(6 * i_bou, 3, 3, 3) =
                ChStarMatrix33<>(floating_frame_F.GetRot().RotateBack(a_mod.segment(6 * i_bou, 3)));
            V_rmom.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(momen.segment(6 * i_bou, 3));
            O_thetamom.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) = ChStarMatrix33<>(momen.segment(6 * i_bou + 3, 3));
        }

        // inertial damping matrix
        if (use_inertial_damping) {
            Ri_sup = P_W * (O_F * M_red - M_red * O_F) * P_W.transpose() +
                     P_W * (M_red * V * Q - (M_red * V * Q).transpose()) * P_W.transpose() +
                     P_W * (Q.transpose() * V_rmom.transpose() - V_rmom * Q) * P_W.transpose() +
                     O_B * M_red * P_W.transpose() - O_thetamom;
        }

        // inertial stiffness matrix
        if (use_inertial_stiffness) {
            ChVectorDynamic<> h_loc_alpha(bou_mod_coords_w);
            h_loc_alpha = M_red * P_perp * (P_W.transpose() * v_mod);
            ChMatrixDynamic<> Xi_FH_alpha;
            GetXi_UlocT_FH(h_loc_alpha, Xi_FH_alpha);

            ChVectorDynamic<> h_loc_beta(6);
            h_loc_beta = Q * (P_W.transpose() * v_mod);
            ChMatrixDynamic<> Xi_VD_beta;
            GetXi_Uloc_VD(h_loc_beta, Xi_VD_beta);

            ChVectorDynamic<> h_loc_gamma(bou_mod_coords_w);
            h_loc_gamma = Q.transpose() * V.transpose() * M_red * (P_W.transpose() * v_mod);
            ChMatrixDynamic<> Xi_FH_gamma;
            GetXi_UlocT_FH(h_loc_gamma, Xi_FH_gamma);

            ChVectorDynamic<> h_loc_epsilon(6);
            h_loc_epsilon = UTMU_inv_solver.solve(V.transpose() * M_red * (P_W.transpose() * v_mod));
            ChMatrixDynamic<> Xi_VD_epsilon;
            GetXi_Uloc_VD(h_loc_epsilon, Xi_VD_epsilon);

            Ki_sup = P_W * (V_rmom - M_red * V) * UTMU_inv_solver.solve(Xi_FH_alpha) +
                     P_W * (V_rmom - M_red * V) * Q * Xi_VD_beta - P_W * Q.transpose() * Xi_FH_gamma -
                     P_W * P_perp.transpose() * M_red * Xi_VD_epsilon +
                     P_W * (M_red * V_acc - V_iner) * Q * P_W.transpose() +
                     P_W * ((O_F + O_B) * M_red - M_red * O_F + M_red * V * Q - (M_red * V * Q).transpose()) * V * Q *
                         P_W.transpose() +
                     P_W * (Q.transpose() * V_rmom.transpose() - V_rmom * Q) * V * Q * P_W.transpose();
        }
    }
}

void ChModalAssembly::ComputeStiffnessMatrix() {
    if (!is_modal)
        return;

    // material stiffness matrix of reduced superelement
    Km_sup = P_W * (P_perp.transpose() * K_red * P_perp) * P_W.transpose();

    if (use_geometric_stiffness) {
        int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

        double fooT;
        ChState x_mod;       // =[qB; eta]
        ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        x_mod.setZero(bou_mod_coords, nullptr);
        v_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        ChStateDelta u_locred(bou_mod_coords_w, nullptr);
        ChStateDelta e_locred(bou_mod_coords_w, nullptr);
        ChStateDelta edt_locred(bou_mod_coords_w, nullptr);
        this->GetStateLocal(u_locred, e_locred, edt_locred, "projection");

        // local internal forces of reduced superelement
        g_loc = K_red * e_locred;

        // geometrical stiffness matrix of reduced superelement
        ChVectorDynamic<> g_loc_alpha(bou_mod_coords_w);
        g_loc_alpha = P_perp.transpose() * g_loc;
        ChVectorDynamic<> g_loc_beta(6);
        g_loc_beta = UTMU_inv_solver.solve(U_locred.transpose() * g_loc);

        ChVectorDynamic<> h_loc_gamma(6);
        h_loc_gamma = Q * u_locred;  //=u_F

        ChVectorDynamic<> h_loc_epsilon(bou_mod_coords_w);
        h_loc_epsilon = M_red * (P_perp * u_locred);

        ChMatrixDynamic<> V_F1;
        V_F1.setZero(bou_mod_coords_w, 6);
        ChMatrixDynamic<> V_F2;
        V_F2.setZero(bou_mod_coords_w, 6);
        for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
            V_F1.block(6 * i_bou, 3, 3, 3) = -ChStarMatrix33<>(g_loc_alpha.segment(6 * i_bou, 3));
            V_F2.block(6 * i_bou, 3, 3, 3) = ChStarMatrix33<>(u_locred.segment(6 * i_bou, 3));
        }
        ChMatrixDynamic<> Kg1;
        Kg1.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg1 = -P_W * V_F1 * Q * P_W.transpose();

        ChMatrixDynamic<> Xi_FH_alpha;
        GetXi_UlocT_FH(g_loc_alpha, Xi_FH_alpha);
        ChMatrixDynamic<> Kg2;
        Kg2.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg2 = P_W * Q.transpose() * Xi_FH_alpha;

        ChMatrixDynamic<> Xi_VD_beta;
        GetXi_Uloc_VD(g_loc_beta, Xi_VD_beta);
        ChMatrixDynamic<> Kg3;
        Kg3.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg3 = -P_W * P_perp.transpose() * M_red * Xi_VD_beta;

        ChMatrixDynamic<> Xi_VD_gamma;
        GetXi_Uloc_VD(h_loc_gamma, Xi_VD_gamma);
        ChMatrixDynamic<> Kg4;
        Kg4.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg4 = -P_W * P_perp.transpose() * K_red * P_perp * Xi_VD_gamma;

        ChMatrixDynamic<> Xi_FH_epsilon;
        GetXi_UlocT_FH(h_loc_epsilon, Xi_FH_epsilon);
        ChMatrixDynamic<> Kg5;
        Kg5.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg5 = P_W * P_perp.transpose() * K_red * U_locred * (UTMU_inv_solver.solve(Xi_FH_epsilon));

        ChMatrixDynamic<> Kg6;
        Kg6.setZero(bou_mod_coords_w, bou_mod_coords_w);
        Kg6 = P_W * P_perp.transpose() * K_red * P_perp * V_F2 * Q * P_W.transpose();

        Kg_sup.setZero();
        Kg_sup = Kg1 + Kg2 + Kg3 + Kg4 + Kg5 + Kg6;  // more stable in nonlinear static analysis
        // Kg_sup = Kg1 + Kg2 + Kg3;//less stable in nonlinear static analysis
    } else
        Kg_sup.setZero();
}

void ChModalAssembly::ComputeDampingMatrix() {
    // material damping matrix of reduced superelement.
    // neglect the time derivative term dY_dt in the damping model.
    Rm_sup = P_W * P_perp.transpose() * R_red * P_perp * P_W.transpose();
}

void ChModalAssembly::ComputeModalKRMmatrix() {
    // modal mass matrix
    this->modal_M = M_sup;

    // modal stiffness matrix
    this->modal_K = Km_sup;
    if (use_geometric_stiffness)
        this->modal_K += Kg_sup;
    if (use_inertial_stiffness)
        this->modal_K += Ki_sup;

    // modal damping matrix
    this->modal_R = Rm_sup;
    if (use_inertial_damping)
        this->modal_R += Ri_sup;

    // constraint Jacobian matrix
    this->modal_Cq = Cq_red * P_W.transpose();
}

void ChModalAssembly::SetupModalData(int nmodes_reduction) {
    this->n_modes_coords_w = nmodes_reduction;
    this->Setup();

    // Initialize matrices
    P_B1.setZero(n_boundary_coords_w, 6);
    P_B2.setZero(n_boundary_coords_w, n_boundary_coords_w);
    P_I1.setZero(n_internal_coords_w, 6);
    P_I2.setZero(n_internal_coords_w, n_internal_coords_w);
    P_W.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    U_locred.setZero(n_boundary_coords_w + n_modes_coords_w, 6);
    Q.setZero(6, n_boundary_coords_w + n_modes_coords_w);
    P_parallel.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    P_perp.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);

    M_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    K_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    R_red.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Cq_red.setZero(n_boundary_doc_w, n_boundary_coords_w + n_modes_coords_w);

    Km_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Kg_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Rm_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    M_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Ri_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);
    Ki_sup.setZero(n_boundary_coords_w + n_modes_coords_w, n_boundary_coords_w + n_modes_coords_w);

    if (!modal_variables || (modal_variables->Get_ndof() != this->n_modes_coords_w)) {
        // Initialize ChVariable object used for modal variables
        if (modal_variables)
            delete modal_variables;
        modal_variables = new ChVariablesGenericDiagonalMass(this->n_modes_coords_w);
        modal_variables->GetMassDiagonal()
            .setZero();  // diag. mass not needed, the mass will be defined via this->modal_Hblock

        // Initialize the modal_Hblock, which is a ChKblockGeneric referencing all ChVariable items:
        std::vector<ChVariables*> mvars;
        // - for BOUNDARY variables: trick to collect all ChVariable references..
        ChSystemDescriptor temporary_descriptor;
        for (auto& body : bodylist)
            body->InjectVariables(temporary_descriptor);
        for (auto& link : linklist)
            link->InjectVariables(temporary_descriptor);
        for (auto& mesh : meshlist)
            mesh->InjectVariables(temporary_descriptor);
        for (auto& item : otherphysicslist)
            item->InjectVariables(temporary_descriptor);
        mvars = temporary_descriptor.GetVariablesList();
        // - for the MODAL variables:
        mvars.push_back(this->modal_variables);

        // NOTE! Purge the not active variables, so that there is a  1-to-1 mapping
        // between the assembly's matrices this->modal_M, modal_K, modal_R and the modal_Hblock->Get_K() block.
        // In fact the ChKblockGeneric modal_Hblock could also handle the not active vars, but the modal_M, K etc
        // are computed for the active-only variables for simplicity in the Herting transformation.
        std::vector<ChVariables*> mvars_active;
        for (auto mvar : mvars) {
            if (mvar->IsActive())
                mvars_active.push_back(mvar);
        }

        this->modal_Hblock.SetVariables(mvars_active);

        // Initialize vectors to be used with modal coordinates:
        this->modal_q.setZero(this->n_modes_coords_w);
        this->modal_q_dt.setZero(this->n_modes_coords_w);
        this->modal_q_dtdt.setZero(this->n_modes_coords_w);
        this->custom_F_modal.setZero(this->n_modes_coords_w);
        this->custom_F_full.setZero(this->n_boundary_coords_w + this->n_internal_coords_w);

        this->modal_q_old.setZero(this->n_modes_coords_w);
    }
}

bool ChModalAssembly::ComputeModes(const ChModalSolveUndamped& n_modes_settings) {
    m_timer_matrix_assembly.start();
    ChSparseMatrix full_M;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    m_timer_matrix_assembly.stop();

    // SOLVE EIGENVALUE
    this->ComputeModesExternalData(full_M, full_K, full_Cq, n_modes_settings);

    return true;
}

bool ChModalAssembly::ComputeModesExternalData(ChSparseMatrix& full_M,
                                               ChSparseMatrix& full_K,
                                               ChSparseMatrix& full_Cq,
                                               const ChModalSolveUndamped& n_modes_settings) {
    m_timer_setup.start();

    // cannot use more modes than n. of tot coords, if so, clamp
    // int nmodes_clamped = ChMin(nmodes, this->ncoords_w);

    // assert(full_M.rows() == this->ncoords_w);
    // assert(full_K.rows() == this->ncoords_w);
    // assert(full_Cq.cols() == this->ncoords_w);

    m_timer_setup.stop();

    // SOLVE EIGENVALUE
    // for undamped system (use generalized constrained eigen solver)
    // - Must work with large dimension and sparse matrices only
    // - Must work also in free-free cases, with 6 rigid body modes at 0 frequency.
    m_timer_modal_solver_call.start();
    n_modes_settings.Solve(full_M, full_K, full_Cq, this->modes_V, this->modes_eig, this->modes_freq);
    m_timer_modal_solver_call.stop();

    m_timer_setup.start();

    this->modes_damping_ratio.setZero(this->modes_freq.rows());

    m_timer_setup.stop();

    return true;
}

bool ChModalAssembly::ComputeModesDamped(const ChModalSolveDamped& n_modes_settings) {
    m_timer_setup.start();

    this->SetupInitial();
    this->Setup();
    this->Update();

    m_timer_setup.stop();

    m_timer_matrix_assembly.start();

    ChSparseMatrix full_M;
    ChSparseMatrix full_R;
    ChSparseMatrix full_K;
    ChSparseMatrix full_Cq;

    this->GetSubassemblyMassMatrix(&full_M);
    this->GetSubassemblyDampingMatrix(&full_R);
    this->GetSubassemblyStiffnessMatrix(&full_K);
    this->GetSubassemblyConstraintJacobianMatrix(&full_Cq);

    m_timer_matrix_assembly.stop();

    // SOLVE QUADRATIC EIGENVALUE
    // for damped system (use quadratic constrained eigen solver)
    // - Must work with large dimension and sparse matrices only
    // - Must work also in free-free cases, with 6 rigid body modes at 0 frequency.
    m_timer_modal_solver_call.start();
    n_modes_settings.Solve(full_M, full_R, full_K, full_Cq, this->modes_V, this->modes_eig, this->modes_freq,
                           this->modes_damping_ratio);
    m_timer_modal_solver_call.stop();

    m_timer_setup.start();
    this->Setup();
    m_timer_setup.stop();

    return true;
}

void ChModalAssembly::SetFullStateWithModeOverlay(int n_mode, double phase, double amplitude) {
    if (n_mode >= this->modes_V.cols()) {
        this->Update();
        throw ChException("Error: mode " + std::to_string(n_mode) + " is beyond the " +
                          std::to_string(this->modes_V.cols()) + " computed eigenvectors.");
    }

    if (this->modes_V.rows() != this->ncoords_w) {
        this->Update();
        return;
    }

    double fooT = 0;
    ChState assembly_x_new;
    ChStateDelta assembly_v;
    ChStateDelta assembly_Dx_loc;
    ChStateDelta assembly_Dx;

    assembly_x_new.setZero(this->ncoords, nullptr);
    assembly_v.setZero(this->ncoords_w, nullptr);
    assembly_Dx_loc.setZero(this->ncoords_w, nullptr);
    assembly_Dx.setZero(this->ncoords_w, nullptr);

    // pick the nth eigenvector in local reference F
    assembly_Dx_loc = sin(phase) * amplitude * this->modes_V.col(n_mode).real() +
                      cos(phase) * amplitude * this->modes_V.col(n_mode).imag();

    // transform the above local increment in F to the original mixed basis,
    // then it can be accumulated to modes_assembly_x0 to update the position.
    for (int i = 0; i < ncoords_w / 6; ++i) {
        assembly_Dx.segment(6 * i, 3) = floating_frame_F.GetA() * assembly_Dx_loc.segment(6 * i, 3);  // translation
        assembly_Dx.segment(6 * i + 3, 3) = assembly_Dx_loc.segment(6 * i + 3, 3);                    // rotation
    }

    this->IntStateIncrement(0, assembly_x_new, this->modes_assembly_x0, 0,
                            assembly_Dx);  // x += amplitude * eigenvector

    this->IntStateScatter(0, assembly_x_new, 0, assembly_v, fooT, true);

    this->Update();
}

void ChModalAssembly::SetInternalStateWithModes(bool full_update) {
    if (!this->is_modal)
        return;

    int bou_int_coords = this->n_boundary_coords + this->n_internal_coords;
    int bou_int_coords_w = this->n_boundary_coords_w + this->n_internal_coords_w;
    int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
    int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

    if (this->Psi.rows() != bou_int_coords_w || this->Psi.cols() != bou_mod_coords_w)
        return;

    bool do_linear_update = true;
    bool rigidbody_mode_test = false;

    double fooT;
    ChState x_mod;       // =[qB; eta]
    ChStateDelta v_mod;  // =[qB_dt; eta_dt]
    x_mod.setZero(bou_mod_coords, nullptr);
    v_mod.setZero(bou_mod_coords_w, nullptr);
    this->IntStateGather(0, x_mod, 0, v_mod, fooT);

    if (do_linear_update) {  // Update w.r.t. the undeformed configuration

        ChStateDelta u_locred(bou_mod_coords_w, nullptr);
        ChStateDelta e_locred(bou_mod_coords_w, nullptr);
        ChStateDelta edt_locred(bou_mod_coords_w, nullptr);
        this->GetStateLocal(u_locred, e_locred, edt_locred, "projection");

        ChStateDelta Dx_internal_local;  // =[delta_qI^bar]
        Dx_internal_local.setZero(this->n_internal_coords_w, nullptr);
        Dx_internal_local.segment(0, this->n_internal_coords_w) =
            Psi_S * e_locred.segment(0, n_boundary_coords_w) +
            Psi_D * e_locred.segment(n_boundary_coords_w, n_modes_coords_w);

        ChState assembly_x_new;  // =[qB_new; qI_new]
        assembly_x_new.setZero(bou_int_coords, nullptr);
        assembly_x_new.head(n_boundary_coords) = x_mod.head(n_boundary_coords);

        for (int i_int = 0; i_int < n_internal_coords_w / 6; i_int++) {
            int offset_x = n_boundary_coords + 7 * i_int;
            ChVector<> r_IF0 = floating_frame_F0.GetA().transpose() *
                               (modes_assembly_x0.segment(offset_x, 3) - floating_frame_F0.GetPos().eigen());
            ChVector<> r_I =
                floating_frame_F.GetPos() + floating_frame_F.GetA() * (r_IF0 + Dx_internal_local.segment(6 * i_int, 3));
            assembly_x_new.segment(offset_x, 3) = r_I.eigen();

            ChQuaternion<> q_delta;
            q_delta.Q_from_Rotv(Dx_internal_local.segment(6 * i_int + 3, 3));
            ChQuaternion<> quat_int0 = modes_assembly_x0.segment(offset_x + 3, 4);
            ChQuaternion<> q_refrot = floating_frame_F0.GetRot().GetConjugate() * quat_int0;
            // ChQuaternion<> quat_int = floating_frame_F.GetRot() * q_delta * q_refrot;
            ChQuaternion<> quat_int =
                floating_frame_F.GetRot() * floating_frame_F0.GetRot().GetConjugate() * quat_int0 * q_delta;
            assembly_x_new.segment(offset_x + 3, 4) = quat_int.eigen();
        }

        ChStateDelta assembly_v_new;  // =[qB_dt; qI_dt]
        assembly_v_new.setZero(bou_int_coords_w, nullptr);
        assembly_v_new.segment(0, n_boundary_coords_w) = v_mod.segment(0, n_boundary_coords_w);
        assembly_v_new.segment(n_boundary_coords_w, n_internal_coords_w) =
            P_I2 * (Psi_S * (P_B2.transpose() * v_mod.segment(0, n_boundary_coords_w)) +
                    Psi_D * v_mod.segment(n_boundary_coords_w, n_modes_coords_w));
        // Add the residual term which should be zero in the assumption of small deflections, but finally it affects
        // because Psi_S=-K_II\K_IB is not updated simultaneously according to the current configuration.
        ChVectorDynamic<> vel_F(6);
        vel_F = Q * (P_W.transpose() * v_mod);
        assembly_v_new.segment(n_boundary_coords_w, n_internal_coords_w) +=
            (P_I1 - P_I2 * Psi_S * P_B2.transpose() * P_B1) * vel_F;

        bool needs_temporary_bou_int = this->is_modal;
        if (needs_temporary_bou_int)
            this->is_modal = false;

        // scatter to internal nodes only and update them
        unsigned int displ_x = 0 - this->offset_x;
        unsigned int displ_v = 0 - this->offset_w;
        double T = this->GetChTime();
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatter(displ_x + body->GetOffset_x(), assembly_x_new, displ_v + body->GetOffset_w(),
                                      assembly_v_new, T, full_update);
            else
                body->Update(T, full_update);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatter(displ_x + mesh->GetOffset_x(), assembly_x_new, displ_v + mesh->GetOffset_w(),
                                  assembly_v_new, T, full_update);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatter(displ_x + link->GetOffset_x(), assembly_x_new, displ_v + link->GetOffset_w(),
                                      assembly_v_new, T, full_update);
            else
                link->Update(T, full_update);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateScatter(displ_x + item->GetOffset_x(), assembly_x_new, displ_v + item->GetOffset_w(),
                                      assembly_v_new, T, full_update);
        }

        if (needs_temporary_bou_int)
            this->is_modal = true;

        // store the full state for the computation in next time step
        full_assembly_x_old = assembly_x_new;
        full_assembly_v_old = assembly_v_new;
        modal_q_old = modal_q;

    } else {  // Accumulated update w.r.t. the deformed configration at previous time step

        // This method does NOT work well.

        // double fooT;
        // ChState x_mod;       // =[qB; eta]
        // ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        // x_mod.setZero(bou_mod_coords, nullptr);
        // v_mod.setZero(bou_mod_coords_w, nullptr);
        // this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        // the old state snapshot (modal reduced)
        ChState x0_mod;  // =[qB_old; eta_old]
        x0_mod.setZero(bou_mod_coords, nullptr);
        x0_mod.segment(0, this->n_boundary_coords) = this->full_assembly_x_old.segment(0, this->n_boundary_coords);
        x0_mod.segment(this->n_boundary_coords, this->n_modes_coords_w) =
            modal_q_old.segment(0, this->n_modes_coords_w);

        ChStateDelta assembly_Dx_reduced;  // = [delta_qB; delta_eta]. Note: delta_qB=qB-qB_old, delta_eta=eta-eta_old
        assembly_Dx_reduced.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGetIncrement(0, x_mod, x0_mod, 0, assembly_Dx_reduced);

        // recover the increment of full state (for both boundary and internal nodes)
        ChStateDelta assembly_Dx;  // =[delta_qB; delta_qI]
        assembly_Dx.setZero(bou_int_coords_w, nullptr);
        assembly_Dx.segment(0, n_boundary_coords_w) = assembly_Dx_reduced.segment(0, n_boundary_coords_w);
        assembly_Dx.segment(n_boundary_coords_w, n_internal_coords_w) =
            P_I2 * Psi_S * P_B2.transpose() * assembly_Dx_reduced.segment(0, n_boundary_coords_w) +
            P_I2 * Psi_D * assembly_Dx_reduced.segment(n_boundary_coords_w, n_modes_coords_w);
        // Add the residual term which should be zero in the assumption of small deflections, but finally it affects
        // because Psi_S=-K_II\K_IB is not updated simultaneously according to the current configuration.
        ChVectorDynamic<> delta_pos_F(6);
        delta_pos_F = Q * (P_W.transpose() * assembly_Dx_reduced);
        assembly_Dx.segment(n_boundary_coords_w, n_internal_coords_w) +=
            (P_I1 - P_I2 * Psi_S * P_B2.transpose() * P_B1) * delta_pos_F;

        ChStateDelta assembly_v_new;  // =[qB_dt; qI_dt]
        assembly_v_new.setZero(bou_int_coords_w, nullptr);
        assembly_v_new.segment(0, n_boundary_coords_w) = v_mod.segment(0, n_boundary_coords_w);
        assembly_v_new.segment(n_boundary_coords_w, n_internal_coords_w) =
            P_I2 * Psi_S * P_B2.transpose() * v_mod.segment(0, n_boundary_coords_w) +
            P_I2 * Psi_D * v_mod.segment(n_boundary_coords_w, n_modes_coords_w);
        // Add the residual term which should be zero in the assumption of small deflections, but finally it affects
        // because Psi_S=-K_II\K_IB is not updated simultaneously according to the current configuration.
        ChVectorDynamic<> vel_F(6);
        vel_F = Q * (P_W.transpose() * v_mod);
        assembly_v_new.segment(n_boundary_coords_w, n_internal_coords_w) +=
            (P_I1 - P_I2 * Psi_S * P_B2.transpose() * P_B1) * vel_F;

        bool needs_temporary_bou_int = this->is_modal;
        if (needs_temporary_bou_int)
            this->is_modal = false;

        ChState assembly_x_new;  // =[qB_new; qI_new]
        assembly_x_new.setZero(bou_int_coords, nullptr);
        this->IntStateIncrement(0, assembly_x_new, this->full_assembly_x_old, 0, assembly_Dx);

        // scatter to internal nodes only and update them
        unsigned int displ_x = 0 - this->offset_x;
        unsigned int displ_v = 0 - this->offset_w;
        double T = this->GetChTime();
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatter(displ_x + body->GetOffset_x(), assembly_x_new, displ_v + body->GetOffset_w(),
                                      assembly_v_new, T, full_update);
            else
                body->Update(T, full_update);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatter(displ_x + mesh->GetOffset_x(), assembly_x_new, displ_v + mesh->GetOffset_w(),
                                  assembly_v_new, T, full_update);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatter(displ_x + link->GetOffset_x(), assembly_x_new, displ_v + link->GetOffset_w(),
                                      assembly_v_new, T, full_update);
            else
                link->Update(T, full_update);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateScatter(displ_x + item->GetOffset_x(), assembly_x_new, displ_v + item->GetOffset_w(),
                                      assembly_v_new, T, full_update);
        }

        if (needs_temporary_bou_int)
            this->is_modal = true;

        // store the full state for the computation in next time step
        full_assembly_x_old = assembly_x_new;
        full_assembly_v_old = assembly_v_new;
        modal_q_old = modal_q;
    }

    if (rigidbody_mode_test) {
        ChMatrixDynamic<> check = (P_I1 - P_I2 * Psi_S * P_B2.transpose() * P_B1);
        GetLog() << "Time: " << GetChTime() << "\t";
        GetLog() << "rigidbody mode check==0?\t" << check.norm() << "\n";
    }
}

void ChModalAssembly::SetFullStateReset() {
    if (this->modes_assembly_x0.rows() != this->ncoords)
        return;

    double fooT = 0;
    ChStateDelta assembly_v;

    assembly_v.setZero(this->ncoords_w, nullptr);

    this->IntStateScatter(0, this->modes_assembly_x0, 0, assembly_v, fooT, true);

    this->Update();
}

void ChModalAssembly::SetInternalNodesUpdate(bool mflag) {
    this->internal_nodes_update = mflag;
}

//---------------------------------------------------------------------------------------

// Note: removing items from the assembly incurs linear time cost

void ChModalAssembly::AddInternalBody(std::shared_ptr<ChBody> body) {
    assert(std::find(std::begin(internal_bodylist), std::end(internal_bodylist), body) == internal_bodylist.end());
    assert(body->GetSystem() == nullptr);  // should remove from other system before adding here

    // set system and also add collision models to system
    body->SetSystem(system);
    internal_bodylist.push_back(body);

    ////system->is_initialized = false;  // Not needed, unless/until ChBody::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalBody(std::shared_ptr<ChBody> body) {
    auto itr = std::find(std::begin(internal_bodylist), std::end(internal_bodylist), body);
    assert(itr != internal_bodylist.end());

    internal_bodylist.erase(itr);
    body->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalLink(std::shared_ptr<ChLinkBase> link) {
    assert(std::find(std::begin(internal_linklist), std::end(internal_linklist), link) == internal_linklist.end());

    link->SetSystem(system);
    internal_linklist.push_back(link);

    ////system->is_initialized = false;  // Not needed, unless/until ChLink::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalLink(std::shared_ptr<ChLinkBase> link) {
    auto itr = std::find(std::begin(internal_linklist), std::end(internal_linklist), link);
    assert(itr != internal_linklist.end());

    internal_linklist.erase(itr);
    link->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalMesh(std::shared_ptr<fea::ChMesh> mesh) {
    assert(std::find(std::begin(internal_meshlist), std::end(internal_meshlist), mesh) == internal_meshlist.end());

    mesh->SetSystem(system);
    internal_meshlist.push_back(mesh);

    system->is_initialized = false;
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalMesh(std::shared_ptr<fea::ChMesh> mesh) {
    auto itr = std::find(std::begin(internal_meshlist), std::end(internal_meshlist), mesh);
    assert(itr != internal_meshlist.end());

    internal_meshlist.erase(itr);
    mesh->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternalOtherPhysicsItem(std::shared_ptr<ChPhysicsItem> item) {
    assert(!std::dynamic_pointer_cast<ChBody>(item));
    assert(!std::dynamic_pointer_cast<ChShaft>(item));
    assert(!std::dynamic_pointer_cast<ChLinkBase>(item));
    assert(!std::dynamic_pointer_cast<ChMesh>(item));
    assert(std::find(std::begin(internal_otherphysicslist), std::end(internal_otherphysicslist), item) ==
           internal_otherphysicslist.end());
    assert(item->GetSystem() == nullptr);  // should remove from other system before adding here

    // set system and also add collision models to system
    item->SetSystem(system);
    internal_otherphysicslist.push_back(item);

    ////system->is_initialized = false;  // Not needed, unless/until ChPhysicsItem::SetupInitial does something
    system->is_updated = false;
}

void ChModalAssembly::RemoveInternalOtherPhysicsItem(std::shared_ptr<ChPhysicsItem> item) {
    auto itr = std::find(std::begin(internal_otherphysicslist), std::end(internal_otherphysicslist), item);
    assert(itr != internal_otherphysicslist.end());

    internal_otherphysicslist.erase(itr);
    item->SetSystem(nullptr);

    system->is_updated = false;
}

void ChModalAssembly::AddInternal(std::shared_ptr<ChPhysicsItem> item) {
    if (auto body = std::dynamic_pointer_cast<ChBody>(item)) {
        AddInternalBody(body);
        return;
    }

    if (auto link = std::dynamic_pointer_cast<ChLinkBase>(item)) {
        AddInternalLink(link);
        return;
    }

    if (auto mesh = std::dynamic_pointer_cast<fea::ChMesh>(item)) {
        AddInternalMesh(mesh);
        return;
    }

    AddInternalOtherPhysicsItem(item);
}

void ChModalAssembly::RemoveInternal(std::shared_ptr<ChPhysicsItem> item) {
    if (auto body = std::dynamic_pointer_cast<ChBody>(item)) {
        RemoveInternalBody(body);
        return;
    }

    if (auto link = std::dynamic_pointer_cast<ChLinkBase>(item)) {
        RemoveInternalLink(link);
        return;
    }

    if (auto mesh = std::dynamic_pointer_cast<fea::ChMesh>(item)) {
        RemoveInternalMesh(mesh);
        return;
    }

    RemoveInternalOtherPhysicsItem(item);
}

void ChModalAssembly::RemoveAllInternalBodies() {
    for (auto& body : internal_bodylist) {
        body->SetSystem(nullptr);
    }
    internal_bodylist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalLinks() {
    for (auto& link : internal_linklist) {
        link->SetSystem(nullptr);
    }
    internal_linklist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalMeshes() {
    for (auto& mesh : internal_meshlist) {
        mesh->SetSystem(nullptr);
    }
    internal_meshlist.clear();

    if (system)
        system->is_updated = false;
}

void ChModalAssembly::RemoveAllInternalOtherPhysicsItems() {
    for (auto& item : internal_otherphysicslist) {
        item->SetSystem(nullptr);
    }
    internal_otherphysicslist.clear();

    if (system)
        system->is_updated = false;
}

// -----------------------------------------------------------------------------

void ChModalAssembly::GetSubassemblyMassMatrix(ChSparseMatrix* M) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the M part only
    KRMmatricesLoad(0, 0, 1.0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(1.0);

    // Fill system-level M matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, M, nullptr, nullptr, nullptr, nullptr, false, false);
    // M->makeCompressed();
}

void ChModalAssembly::GetSubassemblyStiffnessMatrix(ChSparseMatrix* K) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the K part only
    this->KRMmatricesLoad(1.0, 0, 0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(0.0);

    // Fill system-level K matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, K, nullptr, nullptr, nullptr, nullptr, false, false);
    // K->makeCompressed();
}

void ChModalAssembly::GetSubassemblyDampingMatrix(ChSparseMatrix* R) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all KRM matrices with the R part only
    this->KRMmatricesLoad(0, 1.0, 0);
    // For ChVariable objects without a ChKblock, but still with a mass:
    temp_descriptor.SetMassFactor(0.0);

    // Fill system-level R matrix
    temp_descriptor.ConvertToMatrixForm(nullptr, R, nullptr, nullptr, nullptr, nullptr, false, false);
    // R->makeCompressed();
}

void ChModalAssembly::GetSubassemblyConstraintJacobianMatrix(ChSparseMatrix* Cq) {
    this->SetupInitial();
    this->Setup();
    this->Update();

    ChSystemDescriptor temp_descriptor;

    this->InjectVariables(temp_descriptor);
    this->InjectKRMmatrices(temp_descriptor);
    this->InjectConstraints(temp_descriptor);

    // Load all jacobian matrices
    this->ConstraintsLoadJacobians();

    // Fill system-level R matrix
    temp_descriptor.ConvertToMatrixForm(Cq, nullptr, nullptr, nullptr, nullptr, nullptr, false, false);
    // Cq->makeCompressed();
}

void ChModalAssembly::DumpSubassemblyMatrices(bool save_M, bool save_K, bool save_R, bool save_Cq, const char* path) {
    char filename[300];
    const char* numformat = "%.12g";

    if (save_M) {
        ChSparseMatrix mM;
        this->GetSubassemblyMassMatrix(&mM);
        sprintf(filename, "%s%s", path, "_M.dat");
        ChStreamOutAsciiFile file_M(filename);
        file_M.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mM, file_M);
    }
    if (save_K) {
        ChSparseMatrix mK;
        this->GetSubassemblyStiffnessMatrix(&mK);
        sprintf(filename, "%s%s", path, "_K.dat");
        ChStreamOutAsciiFile file_K(filename);
        file_K.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mK, file_K);
    }
    if (save_R) {
        ChSparseMatrix mR;
        this->GetSubassemblyDampingMatrix(&mR);
        sprintf(filename, "%s%s", path, "_R.dat");
        ChStreamOutAsciiFile file_R(filename);
        file_R.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mR, file_R);
    }
    if (save_Cq) {
        ChSparseMatrix mCq;
        this->GetSubassemblyConstraintJacobianMatrix(&mCq);
        sprintf(filename, "%s%s", path, "_Cq.dat");
        ChStreamOutAsciiFile file_Cq(filename);
        file_Cq.SetNumFormat(numformat);
        StreamOutSparseMatlabFormat(mCq, file_Cq);
    }
}

// -----------------------------------------------------------------------------

void ChModalAssembly::SetSystem(ChSystem* m_system) {
    ChAssembly::SetSystem(m_system);  // parent

    for (auto& body : internal_bodylist) {
        body->SetSystem(m_system);
    }
    for (auto& link : internal_linklist) {
        link->SetSystem(m_system);
    }
    for (auto& mesh : internal_meshlist) {
        mesh->SetSystem(m_system);
    }
    for (auto& item : internal_otherphysicslist) {
        item->SetSystem(m_system);
    }
}

void ChModalAssembly::SyncCollisionModels() {
    ChAssembly::SyncCollisionModels();  // parent

    for (auto& body : internal_bodylist) {
        body->SyncCollisionModels();
    }
    for (auto& link : internal_linklist) {
        link->SyncCollisionModels();
    }
    for (auto& mesh : internal_meshlist) {
        mesh->SyncCollisionModels();
    }
    for (auto& item : internal_otherphysicslist) {
        item->SyncCollisionModels();
    }
}

// -----------------------------------------------------------------------------
// UPDATING ROUTINES

void ChModalAssembly::SetupInitial() {
    ChAssembly::SetupInitial();  // parent

    for (int ip = 0; ip < internal_bodylist.size(); ++ip) {
        internal_bodylist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_linklist.size(); ++ip) {
        internal_linklist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_meshlist.size(); ++ip) {
        internal_meshlist[ip]->SetupInitial();
    }
    for (int ip = 0; ip < internal_otherphysicslist.size(); ++ip) {
        internal_otherphysicslist[ip]->SetupInitial();
    }
}

// Count all bodies, links, meshes, and other physics items.
// Set counters (DOF, num constraints, etc) and offsets.
void ChModalAssembly::Setup() {
    ChAssembly::Setup();  // parent

    n_boundary_bodies = nbodies;
    n_boundary_links = nlinks;
    n_boundary_meshes = nmeshes;
    n_boundary_physicsitems = nphysicsitems;
    n_boundary_coords = ncoords;
    n_boundary_coords_w = ncoords_w;
    n_boundary_doc = ndoc;
    n_boundary_doc_w = ndoc_w;
    n_boundary_doc_w_C = ndoc_w_C;
    n_boundary_doc_w_D = ndoc_w_D;
    n_boundary_sysvars = nsysvars;
    n_boundary_sysvars_w = nsysvars_w;
    n_boundary_dof = ndof;

    n_internal_bodies = 0;
    n_internal_links = 0;
    n_internal_meshes = 0;
    n_internal_physicsitems = 0;
    n_internal_coords = 0;
    n_internal_coords_w = 0;
    n_internal_doc = 0;
    n_internal_doc_w = 0;
    n_internal_doc_w_C = 0;
    n_internal_doc_w_D = 0;

    // For the "internal" items:
    //

    for (auto& body : internal_bodylist) {
        if (body->GetBodyFixed()) {
            // throw ChException("Cannot use a fixed body as internal");
        } else if (body->GetSleeping()) {
            // throw ChException("Cannot use a sleeping body as internal");
        } else {
            n_internal_bodies++;

            body->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
            body->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
            body->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

            body->Setup();  // currently, no-op

            n_internal_coords += body->GetDOF();
            n_internal_coords_w += body->GetDOF_w();
            n_internal_doc_w += body->GetDOC();  // not really needed since ChBody introduces no constraints
        }
    }

    for (auto& link : internal_linklist) {
        if (link->IsActive()) {
            n_internal_links++;

            link->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
            link->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
            link->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

            link->Setup();  // compute DOFs etc. and sets the offsets also in child items, if any

            n_internal_coords += link->GetDOF();
            n_internal_coords_w += link->GetDOF_w();
            n_internal_doc_w += link->GetDOC();
            n_internal_doc_w_C += link->GetDOC_c();
            n_internal_doc_w_D += link->GetDOC_d();
        }
    }

    for (auto& mesh : internal_meshlist) {
        n_internal_meshes++;

        mesh->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
        mesh->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
        mesh->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

        mesh->Setup();  // compute DOFs and iteratively call Setup for child items

        n_internal_coords += mesh->GetDOF();
        n_internal_coords_w += mesh->GetDOF_w();
        n_internal_doc_w += mesh->GetDOC();
        n_internal_doc_w_C += mesh->GetDOC_c();
        n_internal_doc_w_D += mesh->GetDOC_d();
    }

    for (auto& item : internal_otherphysicslist) {
        n_internal_physicsitems++;

        item->SetOffset_x(this->offset_x + n_boundary_coords + n_internal_coords);
        item->SetOffset_w(this->offset_w + n_boundary_coords_w + n_internal_coords_w);
        item->SetOffset_L(this->offset_L + n_boundary_doc_w + n_internal_doc_w);

        item->Setup();

        n_internal_coords += item->GetDOF();
        n_internal_coords_w += item->GetDOF_w();
        n_internal_doc_w += item->GetDOC();
        n_internal_doc_w_C += item->GetDOC_c();
        n_internal_doc_w_D += item->GetDOC_d();
    }

    n_internal_doc = n_internal_doc_w + n_internal_bodies;  // number of constraints including quaternion constraints.
    n_internal_sysvars =
        n_internal_coords + n_internal_doc;  // total number of variables (coordinates + lagrangian multipliers)
    n_internal_sysvars_w = n_internal_coords_w + n_internal_doc_w;  // total number of variables (with 6 dof per body)
    n_internal_dof = n_internal_coords_w - n_internal_doc_w;

    this->custom_F_full.setZero(this->n_boundary_coords_w + this->n_internal_coords_w);

    // For the modal part:
    //

    // (nothing to count)

    // For the entire assembly:
    //

    if (this->is_modal == false) {
        ncoords = n_boundary_coords + n_internal_coords;
        ncoords_w = n_boundary_coords_w + n_internal_coords_w;
        ndoc = n_boundary_doc + n_internal_doc;
        ndoc_w = n_boundary_doc_w + n_internal_doc_w;
        ndoc_w_C = n_boundary_doc_w_C + n_internal_doc_w_C;
        ndoc_w_D = n_boundary_doc_w_D + n_internal_doc_w_D;
        nsysvars = n_boundary_sysvars + n_internal_sysvars;
        nsysvars_w = n_boundary_sysvars_w + n_internal_sysvars_w;
        ndof = n_boundary_dof + n_internal_dof;
        nbodies += n_internal_bodies;
        nlinks += n_internal_links;
        nmeshes += n_internal_meshes;
        nphysicsitems += n_internal_physicsitems;
    } else {
        ncoords = n_boundary_coords + n_modes_coords_w;  // no need for a n_modes_coords, same as n_modes_coords_w
        ncoords_w = n_boundary_coords_w + n_modes_coords_w;
        ndoc = n_boundary_doc;
        ndoc_w = n_boundary_doc_w;
        ndoc_w_C = n_boundary_doc_w_C;
        ndoc_w_D = n_boundary_doc_w_D;
        nsysvars = n_boundary_sysvars + n_modes_coords_w;  // no need for a n_modes_coords, same as n_modes_coords_w
        nsysvars_w = n_boundary_sysvars_w + n_modes_coords_w;
        ndof = n_boundary_dof + n_modes_coords_w;

        this->custom_F_modal.setZero(this->n_modes_coords_w);
    }
}

// Update all physical items (bodies, links, meshes, etc), including their auxiliary variables.
// Updates all forces (automatic, as children of bodies)
// Updates all markers (automatic, as children of bodies).
void ChModalAssembly::Update(bool update_assets) {
    ChAssembly::Update(update_assets);  // parent

    if (is_modal == false) {
        //// NOTE: do not switch these to range for loops (may want to use OMP for)
        for (int ip = 0; ip < (int)internal_bodylist.size(); ++ip) {
            internal_bodylist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_meshlist.size(); ++ip) {
            internal_meshlist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_otherphysicslist.size(); ++ip) {
            internal_otherphysicslist[ip]->Update(ChTime, update_assets);
        }
        for (int ip = 0; ip < (int)internal_linklist.size(); ++ip) {
            internal_linklist[ip]->Update(ChTime, update_assets);
        }

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);
    } else {
        // If in modal reduction mode, the internal parts would not be updated (actually, these could even be
        // removed) However one still might want to see the internal nodes "moving" during animations,
        //
        // todo:
        // maybe here we can call the original update to consider the geometric nonlinearity,
        // for instance, for tower/blade deflections
        if (this->internal_nodes_update)
            this->SetInternalStateWithModes(update_assets);

        if (m_custom_F_modal_callback)
            m_custom_F_modal_callback->evaluate(this->custom_F_modal, *this);

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);

        if (update_assets)
            this->UpdateFloatingFrameOfReference();
    }
}

void ChModalAssembly::SetNoSpeedNoAcceleration() {
    ChAssembly::SetNoSpeedNoAcceleration();  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->SetNoSpeedNoAcceleration();
        }
        for (auto& link : internal_linklist) {
            link->SetNoSpeedNoAcceleration();
        }
        for (auto& mesh : internal_meshlist) {
            mesh->SetNoSpeedNoAcceleration();
        }
        for (auto& item : internal_otherphysicslist) {
            item->SetNoSpeedNoAcceleration();
        }
    } else {
        this->modal_q_dt.setZero(this->n_modes_coords_w);
        this->modal_q_dtdt.setZero(this->n_modes_coords_w);
    }
}

void ChModalAssembly::GetStateLocal(ChStateDelta& u_locred,
                                    ChStateDelta& e_locred,
                                    ChStateDelta& edt_locred,
                                    const std::string& opt) {
    if (is_modal == false) {
        // to do? not useful for the moment.
        return;
    } else {
        int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

        u_locred.setZero(bou_mod_coords_w, nullptr);    // =u_locred =P_W^T*[\delta qB; \delta eta]
        e_locred.setZero(bou_mod_coords_w, nullptr);    // =e_locred =[qB^bar; eta]
        edt_locred.setZero(bou_mod_coords_w, nullptr);  // =edt_locred =[qB^bar_dt; eta_dt]

        // fetch the state snapshot (modal reduced)
        double fooT;
        ChState x_mod;       // =[qB; eta]
        ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        x_mod.setZero(bou_mod_coords, nullptr);
        v_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        u_locred.tail(n_modes_coords_w) = modal_q;
        for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
            u_locred.segment(6 * i_bou, 3) =
                floating_frame_F.GetRot().RotateBack(x_mod.segment(7 * i_bou, 3)).eigen() -
                floating_frame_F0.GetRot().RotateBack(modes_assembly_x0.segment(7 * i_bou, 3)).eigen();

            ChQuaternion<> q_F0 = floating_frame_F0.GetRot();
            ChQuaternion<> q_F = floating_frame_F.GetRot();
            ChQuaternion<> q_B0 = modes_assembly_x0.segment(7 * i_bou + 3, 4);
            ChQuaternion<> q_B = x_mod.segment(7 * i_bou + 3, 4);
            ChQuaternion<> rel_q = q_B0.GetConjugate() * q_F0 * q_F.GetConjugate() * q_B;
            // u_locred.segment(6 * i_bou + 3, 3) = rel_q.Q_to_Rotv().eigen();

            double delta_rot_angle;
            ChVector<> delta_rot_dir;
            rel_q.Q_to_AngAxis(delta_rot_angle, delta_rot_dir);
            u_locred.segment(6 * i_bou + 3, 3) = delta_rot_angle * delta_rot_dir.eigen();
        }

        if (opt == "definition") {
            // method 1: solved according to the definition

            // local displacement
            e_locred.tail(n_modes_coords_w) = x_mod.segment(n_boundary_coords, n_modes_coords_w);
            for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
                ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
                ChVector<> r_BF0 = floating_frame_F0.GetRot().RotateBack(
                    (modes_assembly_x0.segment(7 * i_bou, 3) - floating_frame_F0.GetPos().eigen()));
                e_locred.segment(6 * i_bou, 3) =
                    (floating_frame_F.GetRot().RotateBack((r_B - floating_frame_F.GetPos())) - r_BF0).eigen();

                ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
                ChQuaternion<> quat_bou0 = modes_assembly_x0.segment(7 * i_bou + 3, 4);
                ChQuaternion<> q_delta = quat_bou0.GetConjugate() * floating_frame_F0.GetRot() *
                                         floating_frame_F.GetRot().GetConjugate() * quat_bou;
                // e_locred.segment(6 * i_bou + 3, 3) = q_delta.Q_to_Rotv().eigen();

                double delta_rot_angle;
                ChVector<> delta_rot_dir;
                q_delta.Q_to_AngAxis(delta_rot_angle, delta_rot_dir);
                e_locred.segment(6 * i_bou + 3, 3) = delta_rot_angle * delta_rot_dir.eigen();
            }

            // local velocity
            edt_locred.tail(n_modes_coords_w) = v_mod.segment(n_boundary_coords_w, n_modes_coords_w);
            for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
                ChVector<> r_B = x_mod.segment(7 * i_bou, 3);
                ChVector<> v_B = v_mod.segment(6 * i_bou, 3);
                ChVector<> r_BF_loc = floating_frame_F.GetRot().RotateBack(r_B - floating_frame_F.GetPos());
                edt_locred.segment(6 * i_bou, 3) =
                    (floating_frame_F.GetRot().RotateBack(v_B - floating_frame_F.GetPos_dt()) +
                     ChStarMatrix33(r_BF_loc) * floating_frame_F.GetWvel_loc())
                        .eigen();

                ChVector<> wloc_B = v_mod.segment(6 * i_bou + 3, 3);
                ChQuaternion<> quat_bou = x_mod.segment(7 * i_bou + 3, 4);
                edt_locred.segment(6 * i_bou + 3, 3) =
                    (wloc_B - quat_bou.RotateBack(floating_frame_F.GetWvel_par())).eigen();
            }

        } else if (opt == "projection") {
            // method 2: using the perpendicular projector to extract the pure elastic deformation and velocity
            //***Below code is more stable?? seems more consistent with the update algorithm of floating frame F

            // local displacement
            e_locred = P_perp * u_locred;

            // local velocity
            edt_locred = P_perp * (P_W.transpose() * v_mod);

        } else {
            GetLog() << "The GetStateLocal() type is specified incorrectly...\n";
            assert(0);
        }
    }
}

void ChModalAssembly::IntStateGather(const unsigned int off_x,
                                     ChState& x,
                                     const unsigned int off_v,
                                     ChStateDelta& v,
                                     double& T) {
    ChAssembly::IntStateGather(off_x, x, off_v, v, T);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGather(displ_x + body->GetOffset_x(), x, displ_v + body->GetOffset_w(), v, T);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGather(displ_x + link->GetOffset_x(), x, displ_v + link->GetOffset_w(), v, T);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGather(displ_x + mesh->GetOffset_x(), x, displ_v + mesh->GetOffset_w(), v, T);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateGather(displ_x + item->GetOffset_x(), x, displ_v + item->GetOffset_w(), v, T);
        }
    } else {
        x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) = this->modal_q;
        v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_q_dt;

        T = GetChTime();
    }
}

void ChModalAssembly::IntStateScatter(const unsigned int off_x,
                                      const ChState& x,
                                      const unsigned int off_v,
                                      const ChStateDelta& v,
                                      const double T,
                                      bool full_update) {
    ChAssembly::IntStateScatter(off_x, x, off_v, v, T, full_update);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatter(displ_x + body->GetOffset_x(), x, displ_v + body->GetOffset_w(), v, T,
                                      full_update);
            else
                body->Update(T, full_update);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatter(displ_x + mesh->GetOffset_x(), x, displ_v + mesh->GetOffset_w(), v, T, full_update);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateScatter(displ_x + item->GetOffset_x(), x, displ_v + item->GetOffset_w(), v, T,
                                      full_update);
            else
                item->Update(T, full_update);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatter(displ_x + link->GetOffset_x(), x, displ_v + link->GetOffset_w(), v, T,
                                      full_update);
            else
                link->Update(T, full_update);
        }

        if (m_custom_F_full_callback)
            m_custom_F_full_callback->evaluate(this->custom_F_full, *this);
    } else {
        this->modal_q = x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w);
        this->modal_q_dt = v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);

        // Update:
        this->Update(full_update);

        if (verbose && full_update)
            GetLog() << "\n";
    }

    ChTime = T;
}

void ChModalAssembly::IntStateGatherAcceleration(const unsigned int off_a, ChStateDelta& a) {
    ChAssembly::IntStateGatherAcceleration(off_a, a);  // parent

    unsigned int displ_a = off_a - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGatherAcceleration(displ_a + body->GetOffset_w(), a);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGatherAcceleration(displ_a + link->GetOffset_w(), a);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGatherAcceleration(displ_a + mesh->GetOffset_w(), a);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateGatherAcceleration(displ_a + item->GetOffset_w(), a);
        }
    } else {
        a.segment(off_a + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_q_dtdt;
    }
}

// From state derivative (acceleration) to system, sometimes might be needed
void ChModalAssembly::IntStateScatterAcceleration(const unsigned int off_a, const ChStateDelta& a) {
    ChAssembly::IntStateScatterAcceleration(off_a, a);  // parent

    unsigned int displ_a = off_a - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatterAcceleration(displ_a + body->GetOffset_w(), a);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatterAcceleration(displ_a + mesh->GetOffset_w(), a);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateScatterAcceleration(displ_a + item->GetOffset_w(), a);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatterAcceleration(displ_a + link->GetOffset_w(), a);
        }
    } else {
        this->modal_q_dtdt = a.segment(off_a + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

// From system to reaction forces (last computed) - some timestepper might need this
void ChModalAssembly::IntStateGatherReactions(const unsigned int off_L, ChVectorDynamic<>& L) {
    ChAssembly::IntStateGatherReactions(off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGatherReactions(displ_L + body->GetOffset_L(), L);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGatherReactions(displ_L + link->GetOffset_L(), L);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGatherReactions(displ_L + mesh->GetOffset_L(), L);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateGatherReactions(displ_L + item->GetOffset_L(), L);
        }
    } else {
        // todo:
        //  there might be reactions in the reduced modal assembly due to the existance of this->modal_Cq
    }
}

// From reaction forces to system, ex. store last computed reactions in ChLinkBase objects for plotting etc.
void ChModalAssembly::IntStateScatterReactions(const unsigned int off_L, const ChVectorDynamic<>& L) {
    ChAssembly::IntStateScatterReactions(off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateScatterReactions(displ_L + body->GetOffset_L(), L);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntStateScatterReactions(displ_L + mesh->GetOffset_L(), L);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateScatterReactions(displ_L + item->GetOffset_L(), L);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateScatterReactions(displ_L + link->GetOffset_L(), L);
        }
    } else {
        // todo:
        //  there might be reactions in the reduced modal assembly due to the existance of this->modal_Cq
    }
}

void ChModalAssembly::IntStateIncrement(const unsigned int off_x,
                                        ChState& x_new,
                                        const ChState& x,
                                        const unsigned int off_v,
                                        const ChStateDelta& Dv) {
    ChAssembly::IntStateIncrement(off_x, x_new, x, off_v, Dv);  // parent

    if (is_modal == false) {
        unsigned int displ_x = off_x - this->offset_x;
        unsigned int displ_v = off_v - this->offset_w;

        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateIncrement(displ_x + body->GetOffset_x(), x_new, x, displ_v + body->GetOffset_w(), Dv);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateIncrement(displ_x + link->GetOffset_x(), x_new, x, displ_v + link->GetOffset_w(), Dv);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntStateIncrement(displ_x + mesh->GetOffset_x(), x_new, x, displ_v + mesh->GetOffset_w(), Dv);
        }

        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateIncrement(displ_x + item->GetOffset_x(), x_new, x, displ_v + item->GetOffset_w(), Dv);
        }
    } else {
        x_new.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) =
            x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) +
            Dv.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntStateGetIncrement(const unsigned int off_x,
                                           const ChState& x_new,
                                           const ChState& x,
                                           const unsigned int off_v,
                                           ChStateDelta& Dv) {
    ChAssembly::IntStateGetIncrement(off_x, x_new, x, off_v, Dv);  // parent

    unsigned int displ_x = off_x - this->offset_x;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntStateGetIncrement(displ_x + body->GetOffset_x(), x_new, x, displ_v + body->GetOffset_w(), Dv);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntStateGetIncrement(displ_x + link->GetOffset_x(), x_new, x, displ_v + link->GetOffset_w(), Dv);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntStateGetIncrement(displ_x + mesh->GetOffset_x(), x_new, x, displ_v + mesh->GetOffset_w(), Dv);
        }

        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntStateGetIncrement(displ_x + item->GetOffset_x(), x_new, x, displ_v + item->GetOffset_w(), Dv);
        }
    } else {
        Dv.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) =
            x_new.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w) -
            x.segment(off_x + this->n_boundary_coords, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntLoadResidual_F(const unsigned int off,  ///< offset in R residual
                                        ChVectorDynamic<>& R,    ///< result: the R residual, R += c*F
                                        const double c)          ///< a scaling factor
{
    ChAssembly::IntLoadResidual_F(off, R, c);  // parent

    unsigned int displ_v = off - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_F(displ_v + body->GetOffset_w(), R, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_F(displ_v + link->GetOffset_w(), R, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_F(displ_v + mesh->GetOffset_w(), R, c);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntLoadResidual_F(displ_v + item->GetOffset_w(), R, c);
        }

        // Add custom forces (applied to the original non reduced system)
        if (!this->custom_F_full.isZero()) {
            R.segment(off, this->n_boundary_coords_w + this->n_internal_coords_w) += c * this->custom_F_full;
        }
    } else {
        int bou_mod_coords = this->n_boundary_coords + this->n_modes_coords_w;
        int bou_mod_coords_w = this->n_boundary_coords_w + this->n_modes_coords_w;

        double fooT;
        ChState x_mod;       // =[qB; eta]
        ChStateDelta v_mod;  // =[qB_dt; eta_dt]
        x_mod.setZero(bou_mod_coords, nullptr);
        v_mod.setZero(bou_mod_coords_w, nullptr);
        this->IntStateGather(0, x_mod, 0, v_mod, fooT);

        // 1-
        // Add elastic forces from current modal deformations
        ChStateDelta u_locred(bou_mod_coords_w, nullptr);
        ChStateDelta e_locred(bou_mod_coords_w, nullptr);
        ChStateDelta edt_locred(bou_mod_coords_w, nullptr);
        this->GetStateLocal(u_locred, e_locred, edt_locred, "projection");

        //// note: - sign
        R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) -=
            c * (P_W * P_perp.transpose() * (this->K_red * e_locred) + this->modal_R * v_mod);

        if (use_quadratic_velocity_term) {
            ChMatrixDynamic<> V;
            ChMatrixDynamic<> O_B;
            ChMatrixDynamic<> O_F;
            V.setZero(bou_mod_coords_w, 6);
            O_B.setZero(bou_mod_coords_w, bou_mod_coords_w);
            O_F.setZero(bou_mod_coords_w, bou_mod_coords_w);

            for (int i_bou = 0; i_bou < n_boundary_coords_w / 6; i_bou++) {
                V.block(6 * i_bou, 3, 3, 3) =
                    ChStarMatrix33<>(floating_frame_F.GetRot().RotateBack(v_mod.segment(6 * i_bou, 3)));
                O_B.block(6 * i_bou + 3, 6 * i_bou + 3, 3, 3) = ChStarMatrix33<>(v_mod.segment(6 * i_bou + 3, 3));
                O_F.block(6 * i_bou, 6 * i_bou, 3, 3) = ChStarMatrix33<>(floating_frame_F.GetWvel_loc());
            }

            // quadratic velocity term
            ChMatrixDynamic<> mat_O = P_W * (O_F + O_B) * M_red * P_W.transpose();
            ChMatrixDynamic<> mat_M = P_W * M_red * V * Q * P_W.transpose();
            g_quad.setZero();
            g_quad = (mat_O + mat_M - mat_M.transpose()) * v_mod;

            //// note: - sign
            R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) -= c * g_quad;
        }

        // 2-
        // Add custom forces (in modal coordinates)
        if (!this->custom_F_modal.isZero())
            R.segment(off + this->n_boundary_coords_w, this->n_modes_coords_w) += c * this->custom_F_modal;

        // 3-
        // Add custom forces (applied to the original non reduced system, and transformed into reduced)
        if (!this->custom_F_full.isZero()) {
            ChVectorDynamic<> F_red;
            F_red.setZero(this->n_boundary_coords_w + this->n_modes_coords_w);
            F_red.head(n_boundary_coords_w) =
                this->custom_F_full.head(this->n_boundary_coords_w) +
                P_B2 * Psi_S.transpose() * P_I2.transpose() * this->custom_F_full.tail(this->n_internal_coords_w);
            F_red.tail(n_modes_coords_w) =
                Psi_D.transpose() * P_I2.transpose() * this->custom_F_full.tail(this->n_internal_coords_w);
            R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) += c * F_red;
        }
    }
}

void ChModalAssembly::IntLoadResidual_Mv(const unsigned int off,      ///< offset in R residual
                                         ChVectorDynamic<>& R,        ///< result: the R residual, R += c*M*v
                                         const ChVectorDynamic<>& w,  ///< the w vector
                                         const double c               ///< a scaling factor
) {
    if (is_modal == false) {
        ChAssembly::IntLoadResidual_Mv(off, R, w, c);  // parent
        unsigned int displ_v = off - this->offset_w;

        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_Mv(displ_v + body->GetOffset_w(), R, w, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_Mv(displ_v + link->GetOffset_w(), R, w, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_Mv(displ_v + mesh->GetOffset_w(), R, w, c);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntLoadResidual_Mv(displ_v + item->GetOffset_w(), R, w, c);
        }
    } else {
        ChVectorDynamic<> w_modal = w.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w);
        R.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) += c * (this->modal_M * w_modal);
    }
}

void ChModalAssembly::IntLoadLumpedMass_Md(const unsigned int off, ChVectorDynamic<>& Md, double& err, const double c) {
    unsigned int displ_v = off - this->offset_w;

    if (is_modal == false) {
        ChAssembly::IntLoadLumpedMass_Md(off, Md, err, c);  // parent

        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadLumpedMass_Md(displ_v + body->GetOffset_w(), Md, err, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadLumpedMass_Md(displ_v + link->GetOffset_w(), Md, err, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadLumpedMass_Md(displ_v + mesh->GetOffset_w(), Md, err, c);
        }
        for (auto& item : internal_otherphysicslist) {
            item->IntLoadLumpedMass_Md(displ_v + item->GetOffset_w(), Md, err, c);
        }
    } else {
        Md.segment(off, this->n_boundary_coords_w + this->n_modes_coords_w) += c * this->modal_M.diagonal();
        err += (Md.sum() -
                Md.diagonal()
                    .sum());  // lumping should not be used when modal reduced assembly has full, nondiagonal modal_M
    }
}

void ChModalAssembly::IntLoadResidual_CqL(const unsigned int off_L,    ///< offset in L multipliers
                                          ChVectorDynamic<>& R,        ///< result: the R residual, R += c*Cq'*L
                                          const ChVectorDynamic<>& L,  ///< the L vector
                                          const double c               ///< a scaling factor
) {
    ChAssembly::IntLoadResidual_CqL(off_L, R, L, c);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadResidual_CqL(displ_L + body->GetOffset_L(), R, L, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadResidual_CqL(displ_L + link->GetOffset_L(), R, L, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadResidual_CqL(displ_L + mesh->GetOffset_L(), R, L, c);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntLoadResidual_CqL(displ_L + item->GetOffset_L(), R, L, c);
        }
    } else {
        // todo:
        //  there might be residual CqL in the reduced modal assembly
    }
}

void ChModalAssembly::IntLoadConstraint_C(const unsigned int off_L,  ///< offset in Qc residual
                                          ChVectorDynamic<>& Qc,     ///< result: the Qc residual, Qc += c*C
                                          const double c,            ///< a scaling factor
                                          bool do_clamp,             ///< apply clamping to c*C?
                                          double recovery_clamp      ///< value for min/max clamping of c*C
) {
    ChAssembly::IntLoadConstraint_C(off_L, Qc, c, do_clamp, recovery_clamp);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadConstraint_C(displ_L + body->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadConstraint_C(displ_L + link->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadConstraint_C(displ_L + mesh->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntLoadConstraint_C(displ_L + item->GetOffset_L(), Qc, c, do_clamp, recovery_clamp);
        }
    } else {
        // todo:
        //  there might be constraint C in the reduced modal assembly
    }
}

void ChModalAssembly::IntLoadConstraint_Ct(const unsigned int off_L,  ///< offset in Qc residual
                                           ChVectorDynamic<>& Qc,     ///< result: the Qc residual, Qc += c*Ct
                                           const double c             ///< a scaling factor
) {
    ChAssembly::IntLoadConstraint_Ct(off_L, Qc, c);  // parent

    unsigned int displ_L = off_L - this->offset_L;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntLoadConstraint_Ct(displ_L + body->GetOffset_L(), Qc, c);
        }
        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntLoadConstraint_Ct(displ_L + link->GetOffset_L(), Qc, c);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->IntLoadConstraint_Ct(displ_L + mesh->GetOffset_L(), Qc, c);
        }
        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntLoadConstraint_Ct(displ_L + item->GetOffset_L(), Qc, c);
        }
    } else {
        // todo:
        //  there might be constraint Ct in the reduced modal assembly
    }
}

void ChModalAssembly::IntToDescriptor(const unsigned int off_v,
                                      const ChStateDelta& v,
                                      const ChVectorDynamic<>& R,
                                      const unsigned int off_L,
                                      const ChVectorDynamic<>& L,
                                      const ChVectorDynamic<>& Qc) {
    ChAssembly::IntToDescriptor(off_v, v, R, off_L, L, Qc);  // parent

    unsigned int displ_L = off_L - this->offset_L;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntToDescriptor(displ_v + body->GetOffset_w(), v, R, displ_L + body->GetOffset_L(), L, Qc);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntToDescriptor(displ_v + link->GetOffset_w(), v, R, displ_L + link->GetOffset_L(), L, Qc);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntToDescriptor(displ_v + mesh->GetOffset_w(), v, R, displ_L + mesh->GetOffset_L(), L, Qc);
        }

        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntToDescriptor(displ_v + item->GetOffset_w(), v, R, displ_L + item->GetOffset_L(), L, Qc);
        }
    } else {
        this->modal_variables->Get_qb() = v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
        this->modal_variables->Get_fb() = R.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w);
    }
}

void ChModalAssembly::IntFromDescriptor(const unsigned int off_v,
                                        ChStateDelta& v,
                                        const unsigned int off_L,
                                        ChVectorDynamic<>& L) {
    ChAssembly::IntFromDescriptor(off_v, v, off_L, L);  // parent

    unsigned int displ_L = off_L - this->offset_L;
    unsigned int displ_v = off_v - this->offset_w;

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            if (body->IsActive())
                body->IntFromDescriptor(displ_v + body->GetOffset_w(), v, displ_L + body->GetOffset_L(), L);
        }

        for (auto& link : internal_linklist) {
            if (link->IsActive())
                link->IntFromDescriptor(displ_v + link->GetOffset_w(), v, displ_L + link->GetOffset_L(), L);
        }

        for (auto& mesh : internal_meshlist) {
            mesh->IntFromDescriptor(displ_v + mesh->GetOffset_w(), v, displ_L + mesh->GetOffset_L(), L);
        }

        for (auto& item : internal_otherphysicslist) {
            if (item->IsActive())
                item->IntFromDescriptor(displ_v + item->GetOffset_w(), v, displ_L + item->GetOffset_L(), L);
        }
    } else {
        v.segment(off_v + this->n_boundary_coords_w, this->n_modes_coords_w) = this->modal_variables->Get_qb();
    }
}

// -----------------------------------------------------------------------------

void ChModalAssembly::InjectVariables(ChSystemDescriptor& mdescriptor) {
    ChAssembly::InjectVariables(mdescriptor);  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->InjectVariables(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectVariables(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectVariables(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectVariables(mdescriptor);
        }
    } else {
        mdescriptor.InsertVariables(this->modal_variables);
    }
}

void ChModalAssembly::InjectConstraints(ChSystemDescriptor& mdescriptor) {
    ChAssembly::InjectConstraints(mdescriptor);  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->InjectConstraints(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectConstraints(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectConstraints(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectConstraints(mdescriptor);
        }
    } else {
        // todo:
        //  there might be constraints for the reduced modal assembly: this->modal_Cq
    }
}

void ChModalAssembly::ConstraintsLoadJacobians() {
    ChAssembly::ConstraintsLoadJacobians();  // parent

    if (is_modal == false) {
        for (auto& body : internal_bodylist) {
            body->ConstraintsLoadJacobians();
        }
        for (auto& link : internal_linklist) {
            link->ConstraintsLoadJacobians();
        }
        for (auto& mesh : internal_meshlist) {
            mesh->ConstraintsLoadJacobians();
        }
        for (auto& item : internal_otherphysicslist) {
            item->ConstraintsLoadJacobians();
        }
    } else {
        // todo:
        //  there might be constraints for the reduced modal assembly: this->modal_Cq
    }
}

void ChModalAssembly::InjectKRMmatrices(ChSystemDescriptor& mdescriptor) {
    if (is_modal == false) {
        ChAssembly::InjectKRMmatrices(mdescriptor);  // parent

        for (auto& body : internal_bodylist) {
            body->InjectKRMmatrices(mdescriptor);
        }
        for (auto& link : internal_linklist) {
            link->InjectKRMmatrices(mdescriptor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->InjectKRMmatrices(mdescriptor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->InjectKRMmatrices(mdescriptor);
        }
    } else {
        mdescriptor.InsertKblock(&this->modal_Hblock);
    }
}

void ChModalAssembly::KRMmatricesLoad(double Kfactor, double Rfactor, double Mfactor) {
    if (is_modal == false) {
        ChAssembly::KRMmatricesLoad(Kfactor, Rfactor, Mfactor);  // parent

        for (auto& body : internal_bodylist) {
            body->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& link : internal_linklist) {
            link->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& mesh : internal_meshlist) {
            mesh->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
        for (auto& item : internal_otherphysicslist) {
            item->KRMmatricesLoad(Kfactor, Rfactor, Mfactor);
        }
    } else {
        ComputeInertialKRMmatrix();
        ComputeStiffnessMatrix();
        ComputeDampingMatrix();
        ComputeModalKRMmatrix();

        this->modal_Hblock.Get_K() = this->modal_K * Kfactor + this->modal_R * Rfactor + this->modal_M * Mfactor;
    }
}

// -----------------------------------------------------------------------------
//  STREAMING - FILE HANDLING

void ChModalAssembly::ArchiveOut(ChArchiveOut& marchive) {
    // version number
    marchive.VersionWrite<ChModalAssembly>();

    // serialize parent class
    ChAssembly::ArchiveOut(marchive);

    // serialize all member data:

    marchive << CHNVP(internal_bodylist, "internal_bodies");
    marchive << CHNVP(internal_linklist, "internal_links");
    marchive << CHNVP(internal_meshlist, "internal_meshes");
    marchive << CHNVP(internal_otherphysicslist, "internal_other_physics_items");
    marchive << CHNVP(is_modal, "is_modal");
    marchive << CHNVP(modal_q, "modal_q");
    marchive << CHNVP(modal_q_dt, "modal_q_dt");
    marchive << CHNVP(modal_q_dtdt, "modal_q_dtdt");
    marchive << CHNVP(custom_F_modal, "custom_F_modal");
    marchive << CHNVP(custom_F_full, "custom_F_full");
    marchive << CHNVP(internal_nodes_update, "internal_nodes_update");
}

void ChModalAssembly::ArchiveIn(ChArchiveIn& marchive) {
    // version number
    /*int version =*/marchive.VersionRead<ChModalAssembly>();

    // deserialize parent class
    ChAssembly::ArchiveIn(marchive);

    // stream in all member data:

    // trick needed because the "AddIntenal...()" functions are required
    std::vector<std::shared_ptr<ChBody>> tempbodies;
    marchive >> CHNVP(tempbodies, "internal_bodies");
    RemoveAllBodies();
    for (auto& body : tempbodies)
        AddInternalBody(body);
    std::vector<std::shared_ptr<ChLink>> templinks;
    marchive >> CHNVP(templinks, "internal_links");
    RemoveAllLinks();
    for (auto& link : templinks)
        AddInternalLink(link);
    std::vector<std::shared_ptr<ChMesh>> tempmeshes;
    marchive >> CHNVP(tempmeshes, "internal_mesh");
    RemoveAllMeshes();
    for (auto& mesh : tempmeshes)
        AddInternalMesh(mesh);
    std::vector<std::shared_ptr<ChPhysicsItem>> tempotherphysics;
    marchive >> CHNVP(tempotherphysics, "internal_other_physics_items");
    RemoveAllOtherPhysicsItems();
    for (auto& mphys : tempotherphysics)
        AddInternalOtherPhysicsItem(mphys);

    marchive >> CHNVP(is_modal, "is_modal");
    marchive >> CHNVP(modal_q, "modal_q");
    marchive >> CHNVP(modal_q_dt, "modal_q_dt");
    marchive >> CHNVP(modal_q_dtdt, "modal_q_dtdt");
    marchive >> CHNVP(custom_F_modal, "custom_F_modal");
    marchive >> CHNVP(custom_F_full, "custom_F_full");
    marchive >> CHNVP(internal_nodes_update, "internal_nodes_update");

    // Recompute statistics, offsets, etc.
    Setup();
}

}  // end namespace modal

}  // end namespace chrono
