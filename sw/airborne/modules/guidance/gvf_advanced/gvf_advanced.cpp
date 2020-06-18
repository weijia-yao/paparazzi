/*
 * Copyright (C) 2020 Hector Garcia de Marina <hgarciad@ucm.es>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file modules/guidance/gvf_advanced/gvf_advanced.cpp
 *
 * Guiding vector field algorithm for 2D and 3D complex trajectories.
 */

#include <iostream>
#include <Eigen/Dense>

#include "gvf_advanced.h"
#include "./trajectories/gvf_advanced_3d_ellipse.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "autopilot.h"

// Control
uint32_t t0 = 0; // We need it for calculting the time lapse delta_T
gvf_advanced_con gvf_advanced_control;

// Trajectory
gvf_advanced_tra gvf_advanced_trajectory;

#if PERIODIC_TELEMETRY
#include "subsystems/datalink/telemetry.h"
static void send_gvf_advanced(struct transport_tx *trans, struct link_device *dev)
{
  // Do not know whether is a good idea to do this check here or to include
  // this plen in gvf_trajectory
  int plen;

  switch (gvf_advanced_trajectory.type) {
    case ELLIPSE_3D:
      plen = 6;
      break;
    default:
      plen = 1;
      break;
  }

  uint8_t traj_type = (uint8_t)gvf_advanced_trajectory.type;

  pprz_msg_send_GVF_ADVANCED(trans, dev, AC_ID, &traj_type,
                             &gvf_advanced_control.s, plen, gvf_advanced_trajectory.p_advanced);
}

static void send_circle_advanced(struct transport_tx *trans, struct link_device *dev)
{
  if (gvf_advanced_trajectory.type == ELLIPSE_3D) {
    pprz_msg_send_CIRCLE(trans, dev, AC_ID, &gvf_advanced_trajectory.p_advanced[0], &gvf_advanced_trajectory.p_advanced[1], &gvf_advanced_trajectory.p_advanced[2]);
  }
}

#endif // PERIODIC TELEMETRY

#ifdef __cplusplus
}
#endif

void gvf_advanced_init(void)
{
  gvf_advanced_control.w = 0;
  gvf_advanced_control.delta_T = 0;
  gvf_advanced_control.s = 1;
  gvf_advanced_control.k_roll = GVF_ADVANCED_CONTROL_KROLL;
  gvf_advanced_control.k_climb = GVF_ADVANCED_CONTROL_KCLIMB;
  gvf_advanced_control.k_psi = GVF_ADVANCED_CONTROL_KPSI;
  gvf_advanced_control.L = GVF_ADVANCED_CONTROL_L;
  gvf_advanced_control.beta = GVF_ADVANCED_CONTROL_BETA;

#if PERIODIC_TELEMETRY
  register_periodic_telemetry(DefaultPeriodic, PPRZ_MSG_ID_GVF_ADVANCED, send_gvf_advanced);
  register_periodic_telemetry(DefaultPeriodic, PPRZ_MSG_ID_CIRCLE, send_circle_advanced);
#endif

}

void gvf_advanced_control_2D(float, Eigen::Vector3f *, Eigen::Matrix3f *);
void gvf_advanced_control_2D(float ktheta, Eigen::Vector3f *Chi2d, Eigen::Matrix3f *J2d)
{
  ktheta += ktheta;
  (*Chi2d).setZero();
  (*J2d).setZero();
}

void gvf_advanced_control_3D(float kx, float ky, float kz, float f1, float f2, float f3, float f1d, float f2d,
                             float f3d, float f1dd, float f2dd, float f3dd)
{
  uint32_t now = get_sys_time_msec();
  gvf_advanced_control.delta_T = now - t0;
  t0 = now;

  if (gvf_advanced_control.delta_T > 300) { // We need at least two iterations for Delta_T
    gvf_advanced_control.w = 0; // Reset w since we assume the algorithm starts
    return;
  }

  float L = gvf_advanced_control.L;
  float beta = gvf_advanced_control.beta;

  Eigen::Vector4f X;
  Eigen::Matrix4f J;

  // Error signals phi_x phi_y and phi_z
  struct EnuCoor_f *pos_enu = stateGetPositionEnu_f();
  float x = pos_enu->x;
  float y = pos_enu->y;
  float z = pos_enu->z;

  float phi1 = L * (x - f1);
  float phi2 = L * (y - f2);
  float phi3 = L * (z - f3);

  // Chi
  X(0) = -f1d * L * L * beta - kx * phi1;
  X(1) = -f2d * L * L * beta - ky * phi2;
  X(2) = -f3d * L * L * beta - kz * phi3;
  X(3) = -L * L + beta * (kx * phi1 * f1d + ky * phi2 * f2d + kz * phi3 * f3d);
  X *= L;

  // Jacobian
  J.setZero();
  J(0, 0) = -kx * L;
  J(1, 1) = -ky * L;
  J(2, 2) = -kz * L;
  J(3, 0) = kx * f1d * beta * L;
  J(3, 1) = ky * f2d * beta * L;
  J(3, 2) = kz * f3d * beta * L;
  J(0, 3) = -(beta * L) * (beta * L * f1dd - kx * f1d);
  J(1, 3) = -(beta * L) * (beta * L * f2dd - ky * f2d);
  J(2, 3) = -(beta * L) * (beta * L * f3dd - kz * f3d);
  J(3, 3) =  beta * beta * (kx * (phi1 * f1dd - L * f1d * f1d) + ky * (phi2 * f2dd - L * f2d * f2d)
                            + kz * (phi3 * f3dd - L * f3d * f3d));
  J *= L;

  float ground_speed = stateGetHorizontalSpeedNorm_f();
  float w_dot = (ground_speed * X(3)) / sqrtf(X(0) * X(0) + X(1) * X(1));

  Eigen::Vector4f xi_dot;
  struct EnuCoor_f *vel_enu = stateGetSpeedEnu_f();
  float course = stateGetHorizontalSpeedDir_f();

  xi_dot << vel_enu->x, vel_enu->y, vel_enu->z, w_dot;

  Eigen::Matrix2f E;
  Eigen::Matrix<float, 2, 4> F;
  Eigen::Matrix<float, 2, 4> Fp;
  Eigen::Matrix4f G;
  Eigen::Matrix4f Gp;
  Eigen::Matrix4f I;
  Eigen::Vector2f h;
  Eigen::Matrix<float, 1, 2> ht;

  h << sinf(course), cosf(course);
  ht = h.transpose();
  I.setIdentity();
  F << 1.0, 0.0, 0.0, 0.0,
  0.0, 1.0, 0.0, 0.0;
  float s = gvf_advanced_control.s;
  E <<   0.0, -1.0 * s,
  1.0 * s,    0.0;
  G = F.transpose() * F;
  Fp = E * F;
  Gp = F.transpose() * E * F;

  Eigen::Matrix<float, 1, 4> Xt = X.transpose();
  Eigen::Vector4f Xh = X / X.norm();
  Eigen::Matrix<float, 1, 4> Xht = Xh.transpose();

  float aux = ht * Fp * X;

  float heading_rate = -1 / (Xt * G * X) * Xt * Gp * (I - Xh * Xht) * J * xi_dot - (gvf_advanced_control.k_psi * aux / sqrtf(Xt * G * X));
  float climbing_rate = (ground_speed * X(2)) / sqrtf(X(0) * X(0) + X(1) * X(1));

  // Low-level control
  if (autopilot_get_mode() == AP_MODE_AUTO2) {

    // Virtual coordinate
    gvf_advanced_control.w += w_dot * gvf_advanced_control.delta_T * 1e-3;

    // Vertical Z coordinate
    v_ctl_mode = V_CTL_MODE_AUTO_CLIMB;
    v_ctl_speed_mode = V_CTL_SPEED_THROTTLE;

    v_ctl_climb_setpoint = gvf_advanced_control.k_climb * climbing_rate; // Setting point for vertical speed

    // Lateral XY coordinates
    lateral_mode = LATERAL_MODE_ROLL;

    struct FloatEulers *att = stateGetNedToBodyEulers_f();

    h_ctl_roll_setpoint =
      -gvf_advanced_control.k_roll * atanf(heading_rate * ground_speed / GVF_ADVANCED_GRAVITY / cosf(att->theta));
    BoundAbs(h_ctl_roll_setpoint, h_ctl_roll_max_setpoint); // Setting point for roll angle
  }
}

// 3D ELLIPSE

bool gvf_advanced_3D_ellipse_XY(float xo, float yo, float r, float zl, float zh, float alpha)
{
  horizontal_mode = HORIZONTAL_MODE_CIRCLE; //  Circle for the 2D GCS

  // Safety first! If the asked altitude is low
  if (zl > zh) {
    zl = zh;
  }
  if (zl < 1 || zh < 1) {
    zl = 10;
    zh = 10;
  }
  if (r < 1) {
    r = 60;
  }

  gvf_advanced_trajectory.type = ELLIPSE_3D;
  gvf_advanced_trajectory.p_advanced[0] = xo;
  gvf_advanced_trajectory.p_advanced[1] = yo;
  gvf_advanced_trajectory.p_advanced[2] = r;
  gvf_advanced_trajectory.p_advanced[3] = zl;
  gvf_advanced_trajectory.p_advanced[4] = zh;
  gvf_advanced_trajectory.p_advanced[5] = alpha * M_PI / 180; // In the GCS we set degrees

  float f1, f2, f3, f1d, f2d, f3d, f1dd, f2dd, f3dd;

  gvf_advanced_3d_ellipse_info(&f1, &f2, &f3, &f1d, &f2d, &f3d, &f1dd, &f2dd, &f3dd);
  gvf_advanced_control_3D(gvf_advanced_3d_ellipse_par.kx, gvf_advanced_3d_ellipse_par.ky, gvf_advanced_3d_ellipse_par.kz, f1, f2, f3, f1d, f2d, f3d, f1dd, f2dd, f3dd);

  return true;
}

bool gvf_advanced_3D_ellipse_wp(uint8_t wp, float r, float zl, float zh, float alpha)
{
  gvf_advanced_3D_ellipse_XY(waypoints[wp].x, waypoints[wp].y, r, zl, zh, alpha);
  return true;
}

bool gvf_advanced_3D_ellipse_wp_delta(uint8_t wp, float r, float alt_center, float delta, float alpha)
{
  float zl = alt_center - delta;
  float zh = alt_center + delta;

  gvf_advanced_3D_ellipse_XY(waypoints[wp].x, waypoints[wp].y, r, zl, zh, alpha);
  return true;
}