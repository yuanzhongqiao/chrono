// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2014 projectchrono.org
// All right reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors:
// =============================================================================
//
// M113 driveline model based on ChShaft objects.
//
// =============================================================================

#ifndef M113_DRIVELINE_BDS_H
#define M113_DRIVELINE_BDS_H

#include "chrono_vehicle/tracked_vehicle/driveline/ChTrackDrivelineBDS.h"

namespace m113 {

class M113_DrivelineBDS : public chrono::vehicle::ChTrackDrivelineBDS {
  public:
    M113_DrivelineBDS(const std::string& name);

    ~M113_DrivelineBDS() {}

    virtual double GetDriveshaftInertia() const override { return m_driveshaft_inertia; }
    virtual double GetDifferentialBoxInertia() const override { return m_differentialbox_inertia; }

    virtual double GetConicalGearRatio() const override { return m_conicalgear_ratio; }
    virtual double GetDifferentialRatio() const override { return m_differential_ratio; }

  private:
    // Shaft inertias.
    static const double m_driveshaft_inertia;
    static const double m_differentialbox_inertia;

    // Gear ratios.
    static const double m_conicalgear_ratio;
    static const double m_differential_ratio;
};

}  // end namespace m113

#endif
