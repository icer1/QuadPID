// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: t -*-

#include <FastSerial.h>
#include <AP_Common.h>
#include <AP_Param.h>
#include <AP_Mount.h>

const AP_Param::GroupInfo AP_Mount::var_info[] PROGMEM = {
	// @Param: MODE
	// @DisplayName: Mount operation mode
	// @Description: Camera or antenna mount operation mode
	// @Values: 0:retract,1:neutral,2:MavLink_targeting,3:RC_targeting,4:GPS_point
	// @User: Standard
    AP_GROUPINFO("MODE",       0, AP_Mount, _mount_mode), // see MAV_MOUNT_MODE at ardupilotmega.h

    // @Param: RETRACT
	// @DisplayName: Mount retract angles
	// @Description: Mount angles when in retract operation mode
	// @Units: centi-Degrees
	// @Range: -18000 17999
	// @Increment: 1
	// @User: Standard
    AP_GROUPINFO("RETRACT",    1, AP_Mount, _retract_angles),

	// @Param: NEUTRAL
	// @DisplayName: Mount neutral angles
	// @Description: Mount angles when in neutral operation mode
	// @Units: centi-Degrees
	// @Range: -18000 17999
	// @Increment: 1
	// @User: Standard
    AP_GROUPINFO("NEUTRAL",    2, AP_Mount, _neutral_angles),

    // @Param: CONTROL
	// @DisplayName: Mount control angles
	// @Description: Mount angles when in MavLink or RC control operation mode
	// @Units: centi-Degrees
	// @Range: -18000 17999
	// @Increment: 1
	// @User: Standard
    AP_GROUPINFO("CONTROL",    3, AP_Mount, _control_angles),

	// @Param: STAB_ROLL
	// @DisplayName: Stabilize mount roll
	// @Description:enable roll stabilisation relative to Earth
	// @Values: 0:Disabled,1:Enabled
	// @User: Standard
    AP_GROUPINFO("STAB_ROLL",  4, AP_Mount, _stab_roll),

	// @Param: STAB_PITCH
	// @DisplayName: Stabilize mount pitch
	// @Description: enable pitch/tilt stabilisation relative to Earth
	// @Values: 0:Disabled,1:Enabled
	// @User: Standard
    AP_GROUPINFO("STAB_PITCH", 5, AP_Mount, _stab_pitch),

	// @Param: STAB_YAW
	// @DisplayName: Stabilize mount yaw
	// @Description: enable yaw/pan stabilisation relative to Earth
	// @Values: 0:Disabled,1:Enabled
	// @User: Standard
    AP_GROUPINFO("STAB_YAW",   6, AP_Mount, _stab_yaw),
    AP_GROUPEND
};

extern RC_Channel_aux* g_rc_function[RC_Channel_aux::k_nr_aux_servo_functions];	// the aux. servo ch. assigned to each function

AP_Mount::AP_Mount(const struct Location *current_loc, GPS *&gps, AP_AHRS *ahrs):
_gps(gps)
{
	_ahrs = ahrs;
	_current_loc = current_loc;

	// startup with the mount retracted
	set_mode(MAV_MOUNT_MODE_RETRACT);

	// default to zero angles
	_retract_angles = Vector3f(0,0,0);
	_neutral_angles = Vector3f(0,0,0);
	_control_angles = Vector3f(0,0,0);

	// default mount type to roll/pitch (which is the most common for copter)
	_mount_type = k_tilt_roll;

	// default manual rc channel to disabled
	_manual_rc = NULL;
	_manual_rc_function = AP_MOUNT_MANUAL_RC_FUNCTION_DISABLED;
}

// Auto-detect the mount gimbal type depending on the functions assigned to the servos
void AP_Mount::update_mount_type()
{
	if ((g_rc_function[RC_Channel_aux::k_mount_roll] == NULL) && (g_rc_function[RC_Channel_aux::k_mount_pitch] != NULL) && (g_rc_function[RC_Channel_aux::k_mount_yaw] != NULL))
	{
		_mount_type = k_pan_tilt;
	}
	if ((g_rc_function[RC_Channel_aux::k_mount_roll] != NULL) && (g_rc_function[RC_Channel_aux::k_mount_pitch] != NULL) && (g_rc_function[RC_Channel_aux::k_mount_yaw] == NULL))
	{
		_mount_type = k_tilt_roll;
	}
	if ((g_rc_function[RC_Channel_aux::k_mount_roll] != NULL) && (g_rc_function[RC_Channel_aux::k_mount_pitch] != NULL) && (g_rc_function[RC_Channel_aux::k_mount_yaw] != NULL))
	{
		_mount_type = k_pan_tilt_roll;
	}
}

//sets the servo angles for retraction, note angles are in degrees
void AP_Mount::set_retract_angles(float roll, float pitch, float yaw)
{
	_retract_angles = Vector3f(roll, pitch, yaw);
}

//sets the servo angles for neutral, note angles are in degrees
void AP_Mount::set_neutral_angles(float roll, float pitch, float yaw)
{
	_neutral_angles = Vector3f(roll, pitch, yaw);
}

//sets the servo angles for MAVLink, note angles are in degrees
void AP_Mount::set_control_angles(float roll, float pitch, float yaw)
{
	_control_angles = Vector3f(roll, pitch, yaw);
}

// used to tell the mount to track GPS location
void AP_Mount::set_GPS_target_location(Location targetGPSLocation)
{
	_target_GPS_location=targetGPSLocation;
}

// This one should be called periodically
void AP_Mount::update_mount_position()
{
	switch((enum MAV_MOUNT_MODE)_mount_mode.get())
	{
		// move mount to a "retracted position" or to a position where a fourth servo can retract the entire mount into the fuselage
		case MAV_MOUNT_MODE_RETRACT:
		{
			Vector3f vec = _retract_angles.get();
			_roll_angle  = vec.x;
			_pitch_angle = vec.y;
			_yaw_angle   = vec.z;
			break;
		}

		// move mount to a neutral position, typically pointing forward
		case MAV_MOUNT_MODE_NEUTRAL:
		{
			Vector3f vec = _neutral_angles.get();
			_roll_angle  = vec.x;
			_pitch_angle = vec.y;
			_yaw_angle   = vec.z;
			break;
		}

		// point to the angles given by a mavlink message
		case MAV_MOUNT_MODE_MAVLINK_TARGETING:
		{
			Vector3f vec = _control_angles.get();
			_roll_control_angle  = radians(vec.x);
			_pitch_control_angle = radians(vec.y);
			_yaw_control_angle   = radians(vec.z);
			stabilize();
			break;
		}

		// RC radio manual angle control, but with stabilization from the AHRS
		case MAV_MOUNT_MODE_RC_TARGETING:
		{
//#define MNT_SPRING_LOADED_JOYSTICK
#ifdef MNT_SPRING_LOADED_JOYSTICK
			// rc_input() takes degrees * 100 units
			G_RC_AUX(k_mount_roll)->rc_input(&_roll_control_angle, _roll_angle*100);
			G_RC_AUX(k_mount_pitch)->rc_input(&_pitch_control_angle, _pitch_angle*100);
			G_RC_AUX(k_mount_yaw)->rc_input(&_yaw_control_angle, _yaw_angle*100);
#else
			// allow pilot input to come directly from an RC_Channel
			if( _manual_rc != NULL ) {
				float manual_angle = radians((float)(4500 - _manual_rc->control_in ) / 100.0);
				switch( _manual_rc_function ) {
					case AP_MOUNT_MANUAL_RC_FUNCTION_ROLL:
						_roll_control_angle = manual_angle;
						break;
					case AP_MOUNT_MANUAL_RC_FUNCTION_PITCH:
						_pitch_control_angle = manual_angle;
						break;
					case AP_MOUNT_MANUAL_RC_FUNCTION_YAW:
						_yaw_control_angle = manual_angle;
						break;
					default:
						// do nothing
						break;
				}
			}else{
				// take pilot's input from RC_Channel_aux
				if (g_rc_function[RC_Channel_aux::k_mount_roll])
					_roll_control_angle  = g_rc_function[RC_Channel_aux::k_mount_roll]->angle_input_rad();
				if (g_rc_function[RC_Channel_aux::k_mount_pitch])
					_pitch_control_angle = g_rc_function[RC_Channel_aux::k_mount_pitch]->angle_input_rad();
				if (g_rc_function[RC_Channel_aux::k_mount_yaw])
					_yaw_control_angle   = g_rc_function[RC_Channel_aux::k_mount_yaw]->angle_input_rad();
			}
#endif
			stabilize();
			break;
		}

		// point mount to a GPS point given by the mission planner
		case MAV_MOUNT_MODE_GPS_POINT:
		{
			if(_gps->fix){
				calc_GPS_target_angle(&_target_GPS_location);
				stabilize();
			}
			break;
		}
		default:
			//do nothing
			break;
	}

	// write the results to the servos
	// closest_limit() takes degrees * 10 units
	G_RC_AUX(k_mount_roll)->closest_limit(_roll_angle*10);
	G_RC_AUX(k_mount_pitch)->closest_limit(_pitch_angle*10);
	G_RC_AUX(k_mount_yaw)->closest_limit(_yaw_angle*10);
}

void AP_Mount::set_mode(enum MAV_MOUNT_MODE mode)
{
	_mount_mode = (int8_t)mode;
}

// Change the configuration of the mount
// triggered by a MavLink packet.
void AP_Mount::configure_msg(mavlink_message_t* msg)
{
	__mavlink_mount_configure_t packet;
	mavlink_msg_mount_configure_decode(msg, &packet);
	if (mavlink_check_target(packet.target_system, packet.target_component)) {
		// not for us
		return;
	}
	set_mode((enum MAV_MOUNT_MODE)packet.mount_mode);
	_stab_pitch = packet.stab_pitch;
	_stab_roll  = packet.stab_roll;
	_stab_yaw   = packet.stab_yaw;
}

// Control the mount (depends on the previously set mount configuration)
// triggered by a MavLink packet.
void AP_Mount::control_msg(mavlink_message_t *msg)
{
	__mavlink_mount_control_t packet;
	mavlink_msg_mount_control_decode(msg, &packet);
	if (mavlink_check_target(packet.target_system, packet.target_component)) {
		// not for us
		return;
	}

	switch ((enum MAV_MOUNT_MODE)_mount_mode.get())
	{
	case MAV_MOUNT_MODE_RETRACT:  // Load and keep safe position (Roll,Pitch,Yaw) from EEPROM and stop stabilization
		set_retract_angles(packet.input_b*0.01, packet.input_a*0.01, packet.input_c*0.01);
		if (packet.save_position)
		{
			_retract_angles.save();
		}
		break;

	case MAV_MOUNT_MODE_NEUTRAL:  //  Load and keep neutral position (Roll,Pitch,Yaw) from EEPROM
		set_neutral_angles(packet.input_b*0.01, packet.input_a*0.01, packet.input_c*0.01);
		if (packet.save_position)
		{
			_neutral_angles.save();
		}
		break;

	case MAV_MOUNT_MODE_MAVLINK_TARGETING:  // Load neutral position and start MAVLink Roll,Pitch,Yaw control with stabilization
		set_control_angles(packet.input_b*0.01, packet.input_a*0.01, packet.input_c*0.01);
		break;

	case MAV_MOUNT_MODE_RC_TARGETING:  // Load neutral position and start RC Roll,Pitch,Yaw control with stabilization
		{
			Vector3f vec = _neutral_angles.get();
			_roll_angle  = vec.x;
			_pitch_angle = vec.y;
			_yaw_angle   = vec.z;
		}
		break;

	case MAV_MOUNT_MODE_GPS_POINT:  // Load neutral position and start to point to Lat,Lon,Alt
		Location targetGPSLocation;
		targetGPSLocation.lat = packet.input_a;
		targetGPSLocation.lng = packet.input_b;
		targetGPSLocation.alt = packet.input_c;
		set_GPS_target_location(targetGPSLocation);
		break;

	case MAV_MOUNT_MODE_ENUM_END:
		break;

	default:
		// do nothing
		break;
	}
}

// Return mount status information (depends on the previously set mount configuration)
// triggered by a MavLink packet.
void AP_Mount::status_msg(mavlink_message_t *msg)
{
	__mavlink_mount_status_t packet;
	mavlink_msg_mount_status_decode(msg, &packet);
	if (mavlink_check_target(packet.target_system, packet.target_component)) {
		// not for us
		return;
	}

	switch ((enum MAV_MOUNT_MODE)_mount_mode.get())
	{
	case MAV_MOUNT_MODE_RETRACT:			// safe position (Roll,Pitch,Yaw) from EEPROM and stop stabilization
	case MAV_MOUNT_MODE_NEUTRAL:			// neutral position (Roll,Pitch,Yaw) from EEPROM
	case MAV_MOUNT_MODE_MAVLINK_TARGETING:	// neutral position and start MAVLink Roll,Pitch,Yaw control with stabilization
	case MAV_MOUNT_MODE_RC_TARGETING:		// neutral position and start RC Roll,Pitch,Yaw control with stabilization
		packet.pointing_b = _roll_angle*100;	///< degrees*100
		packet.pointing_a = _pitch_angle*100;	///< degrees*100
		packet.pointing_c = _yaw_angle*100;		///< degrees*100
		break;
	case MAV_MOUNT_MODE_GPS_POINT:         // neutral position and start to point to Lat,Lon,Alt
		packet.pointing_a = _target_GPS_location.lat;	///< latitude
		packet.pointing_b = _target_GPS_location.lng;	///< longitude
		packet.pointing_c = _target_GPS_location.alt;	///< altitude
		break;
	case MAV_MOUNT_MODE_ENUM_END:
		break;
	}

	// status reply
	// TODO: is COMM_3 correct ?
	mavlink_msg_mount_status_send(MAVLINK_COMM_3, packet.target_system, packet.target_component,
			packet.pointing_a, packet.pointing_b, packet.pointing_c);
}

// Set mount point/region of interest, triggered by mission script commands
void AP_Mount::set_roi_cmd(struct Location *target_loc)
{
	// set the target gps location
	_target_GPS_location = *target_loc;

	// set the mode to GPS tracking mode
	set_mode(MAV_MOUNT_MODE_GPS_POINT);
}

// Set mount configuration, triggered by mission script commands
void AP_Mount::configure_cmd()
{
	// TODO get the information out of the mission command and use it
}

// Control the mount (depends on the previously set mount configuration), triggered by mission script commands
void AP_Mount::control_cmd()
{
	// TODO get the information out of the mission command and use it
}

void
AP_Mount::calc_GPS_target_angle(struct Location *target)
{
	float GPS_vector_x = (target->lng-_current_loc->lng)*cos(ToRad((_current_loc->lat+target->lat)/(t7*2.0)))*.01113195;
	float GPS_vector_y = (target->lat-_current_loc->lat)*.01113195;
	float GPS_vector_z = (target->alt-_current_loc->alt);             // baro altitude(IN CM) should be adjusted to known home elevation before take off (Set altimeter).
	float target_distance = 100.0*sqrt(GPS_vector_x*GPS_vector_x + GPS_vector_y*GPS_vector_y);  // Careful , centimeters here locally. Baro/alt is in cm, lat/lon is in meters.
	_roll_control_angle   = 0;
	_pitch_control_angle  = atan2(GPS_vector_z, target_distance);
	_yaw_control_angle    = atan2(GPS_vector_x, GPS_vector_y);
/*
	// Converts +/- 180 into 0-360.
	if(_yaw_control_angle<0){
		_yaw_control_angle += 2*M_PI;
	}
*/
}

/// Stabilizes mount relative to the Earth's frame
/// Inputs:
///    _roll_control_angle   desired roll       angle in radians,
///    _pitch_control_angle  desired pitch/tilt angle in radians,
///    _yaw_control_angle    desired yaw/pan    angle in radians
/// Outputs:
///    _roll_angle           stabilized roll       angle in degrees,
///    _pitch_angle          stabilized pitch/tilt angle in degrees,
///    _yaw_angle            stabilized yaw/pan    angle in degrees
void
AP_Mount::stabilize()
{
	if (_ahrs) {
		// only do the full 3D frame transform if we are doing yaw control
		if (_stab_yaw) {
			Matrix3f m;             ///< holds 3 x 3 matrix, var is used as temp in calcs
			Matrix3f cam;           ///< Rotation matrix earth to camera. Desired camera from input.
			Matrix3f gimbal_target; ///< Rotation matrix from plane to camera. Then Euler angles to the servos.
			m = _ahrs->get_dcm_matrix();
			m.transpose();
			cam.from_euler(_roll_control_angle, _pitch_control_angle, _yaw_control_angle);
			gimbal_target = m * cam;
			gimbal_target.to_euler(&_roll_angle, &_pitch_angle, &_yaw_angle);
			_roll_angle  = _stab_roll?degrees(_roll_angle):degrees(_roll_control_angle);
			_pitch_angle = _stab_pitch?degrees(_pitch_angle):degrees(_pitch_control_angle);
			_yaw_angle   = degrees(_yaw_angle);
		} else {
			// otherwise base mount roll and pitch on the ahrs
			// roll/pitch attitude, plus any requested angle
			_roll_angle  = degrees(_roll_control_angle);
			_pitch_angle = degrees(_pitch_control_angle);
			_yaw_angle   = degrees(_yaw_control_angle);
			if (_stab_roll) {
				_roll_angle -= degrees(_ahrs->roll);
			}
			if (_stab_pitch) {
				_pitch_angle -= degrees(_ahrs->pitch);
			}
		}
	} else {
		_roll_angle  = degrees(_roll_control_angle);
		_pitch_angle = degrees(_pitch_control_angle);
		_yaw_angle   = degrees(_yaw_control_angle);
	}
}

// set_manual_rc_channel - define which RC_Channel is to be used for manual control
void
AP_Mount::set_manual_rc_channel(RC_Channel* rc)
{
	_manual_rc = rc;
}

// set_manual_rc_channel_function - set whether manual rc channel is disabled, controls roll (1), pitch (2) or yaw (3).
void
AP_Mount::set_manual_rc_channel_function(int8_t fn)
{
	// update scaler if the function has changed
	if( _manual_rc_function != fn ) {
		_manual_rc_function = fn;

		// ensure pilot's input appears in the range 0 ~ 9000 ( 90 degrees * 100 ).  We will subtract 45 degrees later to make the range +-45 degrees
		if( _manual_rc != NULL && fn != AP_MOUNT_MANUAL_RC_FUNCTION_DISABLED ) {
			_manual_rc->set_range( 0, 9000 );
		}
	}
}

// For testing and development. Called in the medium loop.
void
AP_Mount::debug_output()
{ 	Serial3.print("current   -     ");
	Serial3.print("lat ");
	Serial3.print(_current_loc->lat);
	Serial3.print(",lon ");
	Serial3.print(_current_loc->lng);
	Serial3.print(",alt ");
	Serial3.println(_current_loc->alt);

	Serial3.print("gps       -     ");
	Serial3.print("lat ");
	Serial3.print(_gps->latitude);
	Serial3.print(",lon ");
	Serial3.print(_gps->longitude);
	Serial3.print(",alt ");
	Serial3.print(_gps->altitude);
	Serial3.println();

	Serial3.print("target   -      ");
	Serial3.print("lat ");
	Serial3.print(_target_GPS_location.lat);
	Serial3.print(",lon ");
	Serial3.print(_target_GPS_location.lng);
	Serial3.print(",alt ");
	Serial3.print(_target_GPS_location.alt);
	Serial3.print(" hdg to targ ");
	Serial3.print(degrees(_yaw_control_angle));
	Serial3.println();
}
