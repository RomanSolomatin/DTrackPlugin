// Copyright (c) 2017, Advanced Realtime Tracking GmbH
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "DTrackPollThread.h"

// this makes UBT allow windows types as (probably) used by the SDK header
// @todo check if this is indeed the case and remove if not needed
#include "AllowWindowsPlatformTypes.h" 

#include "DTrackSDK.hpp"

// revert above allowed types to UBT default
#include "HideWindowsPlatformTypes.h"
#include "Async.h"

#define LOCTEXT_NAMESPACE "DTrackPlugin"

FDTrackPollThread *FDTrackPollThread::m_runnable = nullptr;



FDTrackPollThread::FDTrackPollThread(const UDTrackComponent *n_client, FDTrackPlugin *n_plugin)
		: m_plugin(n_plugin)
		, m_dtrack2(n_client->m_dtrack_2)
		, m_dtrack_server_ip(TCHAR_TO_UTF8(*n_client->m_dtrack_server_ip))
		, m_dtrack_server_port(n_client->m_dtrack_server_port)
		, m_coordinate_system(n_client->m_coordinate_system)
		, m_stop_counter(0) {

	// well, let's just say I don't know how to use the initializer list on them
	FMatrix &trafo_normal = const_cast<FMatrix &>(m_trafo_normal);

	// This is the rotation matrix for coordinate adoption mode "normal"
	trafo_normal.M[0][0] = 0; trafo_normal.M[0][1] = 1; trafo_normal.M[0][2] = 0; trafo_normal.M[0][3] = 0;
	trafo_normal.M[1][0] = 1; trafo_normal.M[1][1] = 0; trafo_normal.M[1][2] = 0; trafo_normal.M[1][3] = 0;
	trafo_normal.M[2][0] = 0; trafo_normal.M[2][1] = 0; trafo_normal.M[2][2] = 1; trafo_normal.M[2][3] = 0;
	trafo_normal.M[3][0] = 0; trafo_normal.M[3][1] = 0; trafo_normal.M[3][2] = 0; trafo_normal.M[3][3] = 1;

	// transposed is cached
	const_cast<FMatrix &>(m_trafo_normal_transposed) = trafo_normal.GetTransposed();

	// This is the rotation matrix for coordinate adoption mode "power wall"
	FMatrix &trafo_powerwall = const_cast<FMatrix &>(m_trafo_powerwall);

	trafo_powerwall.M[0][0] = 0; trafo_powerwall.M[0][1] = 0; trafo_powerwall.M[0][2] = -1; trafo_powerwall.M[0][3] = 0;
	trafo_powerwall.M[1][0] = 1; trafo_powerwall.M[1][1] = 0; trafo_powerwall.M[1][2] = 0; trafo_powerwall.M[1][3] = 0;
	trafo_powerwall.M[2][0] = 0; trafo_powerwall.M[2][1] = 1; trafo_powerwall.M[2][2] = 0; trafo_powerwall.M[2][3] = 0;
	trafo_powerwall.M[3][0] = 0; trafo_powerwall.M[3][1] = 0; trafo_powerwall.M[3][2] = 0; trafo_powerwall.M[3][3] = 1;

	// transposed is cached
	const_cast<FMatrix &>(m_trafo_powerwall_transposed) = trafo_powerwall.GetTransposed();

	// This is the rotation matrix for coordinate adoption mode "unreal adapted"
	FMatrix &trafo_unreal_adapted = const_cast<FMatrix &>(m_trafo_unreal_adapted);

	trafo_unreal_adapted.M[0][0] = 1; trafo_unreal_adapted.M[0][1] = 0; trafo_unreal_adapted.M[0][2] = 0; trafo_unreal_adapted.M[0][3] = 0;
	trafo_unreal_adapted.M[1][0] = 0; trafo_unreal_adapted.M[1][1] = -1; trafo_unreal_adapted.M[1][2] = 0; trafo_unreal_adapted.M[1][3] = 0;
	trafo_unreal_adapted.M[2][0] = 0; trafo_unreal_adapted.M[2][1] = 0; trafo_unreal_adapted.M[2][2] = 1; trafo_unreal_adapted.M[2][3] = 0;
	trafo_unreal_adapted.M[3][0] = 0; trafo_unreal_adapted.M[3][1] = 0; trafo_unreal_adapted.M[3][2] = 0; trafo_unreal_adapted.M[3][3] = 1;

	// transposed is cached
	const_cast<FMatrix &>(m_trafo_unreal_adapted_transposed) = trafo_unreal_adapted.GetTransposed();

	m_thread = FRunnableThread::Create(this, TEXT("FDTrackPollThread"), 0, TPri_Normal);
}

FDTrackPollThread::~FDTrackPollThread() {

	if (m_thread) {
		delete m_thread;
		m_thread = nullptr;
	}

	m_runnable = nullptr;
}

FDTrackPollThread *FDTrackPollThread::start(const UDTrackComponent *n_client, FDTrackPlugin *n_plugin) {
	
	// Create new instance of thread if it does not exist and the platform supports multi threading
	if (!m_runnable && FPlatformProcess::SupportsMultithreading()) {
		m_runnable = new FDTrackPollThread(n_client, n_plugin);
	}
	return m_runnable;
}

void FDTrackPollThread::interrupt() {

	Stop();
}

void FDTrackPollThread::join() {

	m_thread->WaitForCompletion();
}

bool FDTrackPollThread::Init() {

	// I don't wanna initialize the SDK in here as both SDK and (surprisingly) this Runnable interface 
	// have undocumented threading behavior.
	// The only thing I know for sure to run in the actual thread is Run() 
	return true;
}

// 1 is success
// 0 is failure
uint32 FDTrackPollThread::Run() {

	// Initial wait before starting
	FPlatformProcess::Sleep(0.1);

	if (m_dtrack2) {
		m_dtrack.reset(new DTrackSDK(m_dtrack_server_ip, m_dtrack_server_port));
	} else {
		m_dtrack.reset(new DTrackSDK(m_dtrack_server_port));
	}

	// I don't know when this can occur but I guess it's client
	// port collision with fixed UDP ports
	if (!m_dtrack->isLocalDataPortValid()) {
		m_dtrack.reset();
		return 0;
	}

	// start the tracking via tcp route if applicable
	if (m_dtrack2) {
		if (!m_dtrack->startMeasurement()) {
			if (m_dtrack->getLastServerError() == DTrackSDK::ERR_TIMEOUT) {
				UE_LOG(DTrackPluginLog, Error, TEXT("Could not start tracking, timeout"));
			} else if (m_dtrack->getLastServerError() == DTrackSDK::ERR_NET) {
				UE_LOG(DTrackPluginLog, Error, TEXT("Could not start tracking, network error"));
			} else {
				UE_LOG(DTrackPluginLog, Error, TEXT("Could not start tracking"));
			}
			return 0;
		} 
	} 

	// now go looping until Stop() increases the stop condition
	while (!m_stop_counter.GetValue()) {
		// receive as much as we can
		if (m_dtrack->receive()) {

			m_plugin->begin_injection();

			// treat body info and cache results into plug-in
			handle_bodies();
			handle_flysticks();
			handle_hands();
			handle_human_model();
		
			m_plugin->end_injection();
		}
	}

	if (m_dtrack2) {
		UE_LOG(DTrackPluginLog, Display, TEXT("Stopping DTrack2 measurement."));
		m_dtrack->stopMeasurement();
	}

	m_dtrack.reset();

	return 1;

}

void FDTrackPollThread::Stop() {

	m_stop_counter.Set(1);
}


void FDTrackPollThread::Exit() {

}

void FDTrackPollThread::handle_bodies() {

	const DTrack_Body_Type_d *body = nullptr;
	for (int i = 0; i < m_dtrack->getNumBody(); i++) {  // why do we still use int for those counters?
		body = m_dtrack->getBody(i);
		checkf(body, TEXT("DTrack API error, body address null"));

		if (body->quality > 0) {
			// Quality below zero means the body is not visible to the system right now. I won't call the interface

			FVector translation = from_dtrack_location(body->loc);
			FRotator rotation = from_dtrack_rotation(body->rot);

	//		FScopeLock lock(m_plugin->bodies_mutex());
			m_plugin->inject_body_data(body->id, translation, rotation);
		}
	}
}

void FDTrackPollThread::handle_flysticks() {

	const DTrack_FlyStick_Type_d *flystick = nullptr;
	for (int i = 0; i < m_dtrack->getNumFlyStick(); i++) {
		flystick = m_dtrack->getFlyStick(i);
		checkf(flystick, TEXT("DTrack API error, flystick address null"));

		if (flystick->quality > 0) {
			// Quality below zero means the body is not visible to the system right now. I won't call the interface
			FVector translation = from_dtrack_location(flystick->loc);
			FRotator rotation = from_dtrack_rotation(flystick->rot);

			// create a state vector for the button states
			TArray<int> buttons;
			buttons.SetNumZeroed(flystick->num_button);
			for (int idx = 0; idx < flystick->num_button; idx++) {
				buttons[idx] = flystick->button[idx];
			}

			// create a state vector for the joystick states
			TArray<float> joysticks;  // have to use float as blueprints don't support TArray<double>
			joysticks.SetNumZeroed(flystick->num_joystick);
			for (int idx = 0; idx < flystick->num_joystick; idx++) {
				joysticks[idx] = static_cast<float>(flystick->joystick[idx]);
			}

	//		FScopeLock lock(m_plugin->flystick_mutex());
			m_plugin->inject_flystick_data(flystick->id, translation, rotation, buttons, joysticks);
		}
	}
}

void FDTrackPollThread::handle_hands() {

	const DTrack_Hand_Type_d *hand = nullptr;
	for (int i = 0; i < m_dtrack->getNumHand(); i++) {
		hand = m_dtrack->getHand(i);
		checkf(hand, TEXT("DTrack API error, hand address is null"));

		if (hand->quality > 0) {
			FVector translation = from_dtrack_location(hand->loc);
			FRotator rotation = from_dtrack_rotation(hand->rot);
			TArray<FDTrackFinger> fingers;

			for (int j = 0; j < hand->nfinger; j++) {
				FDTrackFinger finger;
				switch (j) {     // this is mostly to allow for the blueprint to be a 
								 // little more expressive than using assumptions about the index' meaning
					case 0: finger.m_type = EDTrackFingerType::FT_Thumb; break;
					case 1: finger.m_type = EDTrackFingerType::FT_Index; break;
					case 2: finger.m_type = EDTrackFingerType::FT_Middle; break;
					case 3: finger.m_type = EDTrackFingerType::FT_Ring; break;
					case 4: finger.m_type = EDTrackFingerType::FT_Pinky; break;
				}

				finger.m_location = from_dtrack_location(hand->finger[j].loc);
				finger.m_rotation = from_dtrack_rotation(hand->finger[j].rot);
				finger.m_tip_radius = hand->finger[j].radiustip;
				finger.m_inner_phalanx_length = hand->finger[j].lengthphalanx[2];
				finger.m_middle_phalanx_length = hand->finger[j].lengthphalanx[1];
				finger.m_outer_phalanx_length = hand->finger[j].lengthphalanx[0];
				finger.m_inner_middle_phalanx_angle = hand->finger[j].anglephalanx[1];
				finger.m_middle_outer_phalanx_angle = hand->finger[j].anglephalanx[0];
				fingers.Add(std::move(finger));
			}

	//		FScopeLock lock(m_plugin->hand_mutex());
			m_plugin->inject_hand_data(hand->id, (hand->lr == 1), translation, rotation, fingers);
		}
	}
}

void FDTrackPollThread::handle_human_model() {
	
	const DTrack_Human_Type_d *human = nullptr;
	for (int i = 0; i < m_dtrack->getNumHuman(); i++) {
		human = m_dtrack->getHuman(i);
		checkf(human, TEXT("DTrack API error, human address is null"));

		TArray<FDTrackJoint> joints;

		for (int j = 0; j < human->num_joints; j++) {
			FDTrackJoint joint;
			// I'm not sure if I should check for quality as I don't know if the caller
			// would expect number and order of joints to be relevant/constant.
			// They do carry an ID though so I suppose the caller must be aware of that.
			if (human->joint[j].quality > 0.1) {
				joint.m_id = human->joint[j].id;
				joint.m_location = from_dtrack_location(human->joint[j].loc);
				joint.m_rotation = from_dtrack_rotation(human->joint[j].rot);
				joint.m_angles.Add(human->joint[j].ang[0]);   // well, are they Euler angles of the same rot as above or not?
				joint.m_angles.Add(human->joint[j].ang[1]);
				joint.m_angles.Add(human->joint[j].ang[2]);
				joints.Add(std::move(joint));
			}
		}

	//	FScopeLock lock(m_plugin->human_mutex());
		m_plugin->inject_human_model_data(human->id, joints);
	}
}

// translate a DTrack body location (translation in mm) into Unreal Location (in cm)
FVector FDTrackPollThread::from_dtrack_location(const double(&n_translation)[3]) {

	FVector ret;

	// DTrack coordinates come in mm with either Z or Y being up, which has to be configured by the user.
	// I translate to Unreal's Z being up and cm units.
	switch (m_coordinate_system) {
		default:
		case EDTrackCoordinateSystemType::CST_Normal:
			ret.X = n_translation[1] / 10.0;
			ret.Y = n_translation[0] / 10.0;
			ret.Z = n_translation[2] / 10.0;
			break;
		case EDTrackCoordinateSystemType::CST_Unreal_Adapted:
			ret.X = n_translation[0] / 10.0;
			ret.Y = -n_translation[1] / 10.0;
			ret.Z = n_translation[2] / 10.0;
			break;
		case EDTrackCoordinateSystemType::CST_Powerwall:
			ret.X = -n_translation[2] / 10.0;
			ret.Y = n_translation[0] / 10.0;
			ret.Z = n_translation[1] / 10.0;
			break;
	}

	return ret;
}

// translate a DTrack 3x3 rotation matrix (translation in mm) into Unreal Location (in cm)
FRotator FDTrackPollThread::from_dtrack_rotation(const double(&n_matrix)[9]) {

	// take DTrack matrix and put the values into FMatrix 
	// ( M[RowIndex][ColumnIndex], DTrack matrix comes column-wise )
	FMatrix r;
	r.M[0][0] = n_matrix[0 + 0]; r.M[0][1] = n_matrix[0 + 3]; r.M[0][2] = n_matrix[0 + 6]; r.M[0][3] = 0.0;
	r.M[1][0] = n_matrix[1 + 0]; r.M[1][1] = n_matrix[1 + 3]; r.M[1][2] = n_matrix[1 + 6]; r.M[1][3] = 0.0;
	r.M[2][0] = n_matrix[2 + 0]; r.M[2][1] = n_matrix[2 + 3]; r.M[2][2] = n_matrix[2 + 6]; r.M[2][3] = 0.0;
	r.M[3][0] = 0.0; r.M[3][1] = 0.0; r.M[3][2] = 0.0; r.M[3][3] = 1.0;

	FMatrix r_adapted;

	switch (m_coordinate_system) {
		default:
		case EDTrackCoordinateSystemType::CST_Normal:
			r_adapted = m_trafo_normal * r * m_trafo_normal_transposed;
			break;

		case EDTrackCoordinateSystemType::CST_Unreal_Adapted:
			r_adapted = m_trafo_unreal_adapted * r * m_trafo_unreal_adapted_transposed;
			break;

		case EDTrackCoordinateSystemType::CST_Powerwall:
			r_adapted = m_trafo_powerwall * r * m_trafo_powerwall_transposed;
			break;
	}

	return r_adapted.GetTransposed().Rotator();
}









#undef LOCTEXT_NAMESPACE
