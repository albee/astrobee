/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 *
 * All rights reserved.
 *
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Command line flags
#include <gflags/gflags.h>
#include <gflags/gflags_completions.h>

// Proxy library
#include <perching_arm/perching_arm.h>

// C includes
#include <climits>
#include <cmath>
#include <csignal>

// STL includes
#include <atomic>
#include <chrono>
#include <condition_variable>  // NOLINT
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// Gflag options
DEFINE_string(port, "/dev/ttyUSB0", "Serial device");
DEFINE_uint64(baud, 115200, "Serial baud rate");
DEFINE_string(log, "", "Start logging to file");
DEFINE_double(timeout, 30.0, "Timeout for any action");
DEFINE_double(deg, 2.0, "Degree tolerance for joint completion");
DEFINE_double(perc, 1.0, "Percentage tolerance for gripper completion");
DEFINE_bool(debug, true, "Print debug information");

namespace perching_arm {

// Possible tool states
enum ToolState {
  WAITING_ON_PROX_POS,
  WAITING_ON_DIST_POS,
  WAITING_ON_COMMAND
};

// By default we are waiting on a command
ToolState state_ = WAITING_ON_COMMAND;

// Logfile for data
std::ofstream logfile_;

// The current goal for the given ToolState
double value_ = 0;

// Motors that are enabled
bool en_prox_ = true;
bool en_dist_ = true;
bool return_ = false;

// Cache feedback from the arm
PerchingArmRaw raw_;

// At the heart of the perching arm proxy library is a serial implementation,
// which is run in a standalone thread. This thread calls back to us when there
// is new data to process. We can use this to implement a blocking wait.
std::condition_variable cv_;
std::mutex mutex_;

// Grab an unsigned integer from user input with type and value checking
char InputCharacter() {
  while (true) {
    std::cout << "[MENU] > ";
    std::string input;
    getline(std::cin, input);
    std::stringstream ss(input);
    if (input.length() == 2) {
      std::cerr << "Please enter only a single character" << std::endl;
      continue;
    }
    return input[0];
  }
  return 0;
}

// Handler for ctrl+c in command mode
void SignalHandler(int sig) {
  return_ = true;
  std::cout << std::endl << "Aborted. Returning to menu." << std::endl;
  std::signal(sig, SignalHandler);
}

// Grab an unsigned short from user input with type and value checking
bool InputUnsignedShort(uint16_t &choice, uint16_t min, uint16_t max) {
  void (*old)(int) = std::signal(SIGINT, SignalHandler);
  bool success = true;
  while (true) {
    std::cout << "Input value > ";
    std::string input;
    getline(std::cin, input);
    if (return_) {
      if (std::cin.fail() || std::cin.eof()) std::cin.clear();
      success = false;
      break;
    }
    std::stringstream ss(input);
    if (ss >> choice && choice >= min && choice <= max) break;
    std::cerr << "Number not in range [" << min << ":" << max
              << "], please try again" << std::endl;
  }
  std::signal(SIGINT, old);
  return success;
}

// Grab an unsigned integer from user input with type and value checking
bool InputSignedShort(int16_t &choice, int16_t min, int16_t max) {
  void (*old)(int) = std::signal(SIGINT, SignalHandler);
  bool success = true;
  while (true) {
    std::cout << "Input value > ";
    std::string input;
    getline(std::cin, input);
    if (return_) {
      if (std::cin.fail() || std::cin.eof()) std::cin.clear();
      success = false;
      break;
    }
    std::stringstream ss(input);
    if (ss >> choice && choice >= min && choice <= max) break;
    std::cerr << "Number not in range [" << min << ":" << max
              << "], please try again" << std::endl;
  }
  std::signal(SIGINT, old);
  return success;
}

// Grab a floating point number from user input with type and value checking
bool InputFloat(float &choice, float min, float max) {
  void (*old)(int) = std::signal(SIGINT, SignalHandler);
  bool success = true;
  while (true) {
    std::cout << "Input value > ";
    std::string input;
    getline(std::cin, input);
    if (return_) {
      if (std::cin.fail() || std::cin.eof()) std::cin.clear();
      success = false;
      break;
    }
    std::stringstream ss(input);
    if (ss >> choice && choice >= min && choice <= max) break;
    std::cerr << "Number not in range [" << min << ":" << max
              << "], please try again" << std::endl;
  }
  std::signal(SIGINT, old);
  return success;
}

// Grab a valid
bool InputFile(std::string &choice) {
  void (*old)(int) = std::signal(SIGINT, SignalHandler);
  bool success = true;
  while (true) {
    std::cout << "Input value > ";
    getline(std::cin, choice);
    if (return_) {
      if (std::cin.fail() || std::cin.eof()) std::cin.clear();
      success = false;
      break;
    }
    std::ofstream f(choice.c_str());
    if (f.good()) break;
    std::cerr << "Could not open the specified file " << choice << std::endl;
  }
  std::signal(SIGINT, old);
  return success;
}

// Wait for a timeout
void WaitForResultWithTimeout(ToolState state, int16_t value) {
  // // Set the goal value and the state
  // value_ = static_cast<double>(value);
  // state_ = state;
  // // Now clock this thread while we wait for a result or timeout
  // std::cout << "Waiting on result" << std::endl;
  // std::unique_lock<std::mutex> lk(mutex_);
  // if (cv_.wait_for(lk, std::chrono::duration<double>(FLAGS_timeout)) ==
  //     std::cv_status::no_timeout)
  //   return;
  // // If we get there then we have timed out
  // state_ = WAITING_ON_COMMAND;
  // // Print out the error that occurred
  // std::cerr << "Error: timeout waiting on command to complete" << std::endl;
}

// Asynchronous callback to copy feedback
void DataCallback(PerchingArmRaw const &raw) {
  // Cache the raw data
  raw_ = raw;
  // log data only if the logfile is open
  if (logfile_.is_open()) {
    logfile_ << std::setw(11) << std::setprecision(2);
    logfile_ << raw.prox.load << '\t' << raw.prox.velocity << '\t'
             << (raw.prox.position - 2048) * 0.088 + 180 << '\t'
             << raw.dist.load << '\t' << raw.dist.velocity << '\t'
             << (raw.dist.position - 2048) * 0.088 + 180 << '\t'
             << raw.grip.position << '\t' << raw.grip.load << '\t'
             << raw.current_11v << '\t' << raw.current_5v << '\t'
             << raw.loop_time << '\t' << raw.board_temp << std::endl;
    logfile_.flush();
  }
  // This will store our current value
  double curr = 0, tolv = 0;
  // Decide what we are waiting for
  switch (state_) {
    case WAITING_ON_PROX_POS:
      curr =
          PerchingArm::K_POSITION_DEG * static_cast<double>(raw.prox.position);
      tolv = FLAGS_deg;
      if (FLAGS_debug) std::cout << "[prox] ";
      break;
    case WAITING_ON_DIST_POS:
      curr =
          PerchingArm::K_POSITION_DEG * static_cast<double>(raw.dist.position);
      tolv = FLAGS_deg;
      if (FLAGS_debug) std::cout << "[dist] ";
      break;
    case WAITING_ON_COMMAND:
    default:
      return;
  }
  // If we are not on tolerance yet, then do nothing
  if (FLAGS_debug)
    std::cout << " curr: " << curr << " goal: " << value_ << " tol: " << tolv
              << std::endl;
  if (std::fabs(curr - value_) > tolv) return;
  // Reset the state of the system
  state_ = WAITING_ON_COMMAND;
  // If we get here then we need to unblock and notify
  cv_.notify_all();
}

// Asynchronous callback
void SleepCallback(uint32_t ms) {
  std::cout << "Sleeping for a few seconds" << std::endl;
  std::this_thread::sleep_for(std::chrono::duration<uint32_t, std::milli>(ms));
}

// Simple scale function
double Scale(double scale, int32_t value) {
  return scale * static_cast<double>(value);
}

// Print out success or error
void PrintResult(PerchingArm &arm, PerchingArmResult ret) {
  // Get the message
  std::string msg;
  if (arm.ResultToString(ret, msg))
    std::cout << msg << std::endl;
  else
    std::cerr << msg << std::endl;
}

// Print a nice menu
void PrintMenu() {
  std::cout << "************ VERSION 07/08/2019 *************" << std::endl;
  std::cout << "********** GECKO PERCHING GRIPPER ***********" << std::endl;
  std::cout << "******************** MENU *******************" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[x] Exit                                    *" << std::endl;
  std::cout << "[m] Menu                                    *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  if (!logfile_.is_open())
    std::cout << "[f] Log all raw data (currently disabled)   *" << std::endl;
  else
    std::cout << "[f] Change log file                         *" << std::endl;
  std::cout << "[d] Display latest data                     *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[t] Set tilt position                       *" << std::endl;
  std::cout << "[u] Set tilt velocity                       *" << std::endl;
  std::cout << "[B] Disable tilt motor                      *" << std::endl;
  std::cout << "[A] Enable tilt motor                       *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[p] Set pan position                        *" << std::endl;
  std::cout << "[q] Set pan velocity                        *" << std::endl;
  std::cout << "[Y] Disable pan motor                       *" << std::endl;
  std::cout << "[X] Enable pan motor                        *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[O] Open gripper                            *" << std::endl;
  std::cout << "[C] Close gripper                           *" << std::endl;
  std::cout << "[G] Engage gripper                          *" << std::endl;
  std::cout << "[N] Disengage gripper                       *" << std::endl;
  std::cout << "[L] Lock gripper                            *" << std::endl;
  std::cout << "[U] Unlock gripper                          *" << std::endl;
  std::cout << "[E] Enable auto gripper                     *" << std::endl;
  std::cout << "[D] Disable auto gripper                    *" << std::endl;
  std::cout << "[T] Toggle auto gripper                     *" << std::endl;
  std::cout << "[M] Mark gripper idx                        *" << std::endl;
  std::cout << "[S] Set delay gripper                       *" << std::endl;
  std::cout << "[F] Open experiment gripper                 *" << std::endl;
  std::cout << "[R] Next record gripper                     *" << std::endl;
  std::cout << "[K] Seek record gripper                     *" << std::endl;
  std::cout << "[c] Close experiment gripper                *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[a] Read gripper status                     *" << std::endl;
  std::cout << "[l] Read SD record                          *" << std::endl;
  std::cout << "[o] Check experiment idx                    *" << std::endl;
  std::cout << "[n] Read TOF delay                          *" << std::endl;
  std::cout << "[e] Check error byte                        *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[h] Hard reset of controller Board (FTDI)   *" << std::endl;
  std::cout << "[s] Soft reset of controller Board (SW)     *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "[r] Set raw command (refer to datasheet)    *" << std::endl;
  std::cout << "*********************************************" << std::endl;
  std::cout << "*********************************************" << std::endl;
}

// Print a main menu
bool GetCommand(PerchingArm &arm) {
  // Reset the return state
  return_ = false;
  // Keep looping until we have valid input
  char choice = InputCharacter();
  // Response code
  PerchingArmResult ret;
  // Do something based on the choice
  switch (choice) {
    // Exit
    case 'x': {
      // Close the logfile if one is open
      if (logfile_.is_open()) logfile_.close();
      // Print goodby message and return
      std::cout << "Goodbye." << std::endl;
      return false;
    }
    // Print menu
    case 'm': {
      PrintMenu();
      return true;
    }
    // Change logfile
    case 'f': {
      // Grab a log file
      std::cout << "Input a path to log file " << std::endl;
      std::string file;
      if (!InputFile(file)) return true;
      // Close the logfile if its already opened
      if (logfile_) logfile_.close();
      // Open the new file
      logfile_.open(file, std::ofstream::out | std::ofstream::app);
      return true;
    }
    // Display feedback
    case 'd': {
      std::cout << "PROXIMAL MOTOR (TILT)" << std::endl;
      std::cout << "- Position: "
                << Scale(PerchingArm::K_POSITION_DEG, raw_.prox.position)
                << " deg (raw: " << raw_.prox.position << ")" << std::endl;
      std::cout << "- Velocity: "
                << Scale(PerchingArm::K_VELOCITY_RPM, raw_.prox.velocity)
                << " rpm (raw: " << raw_.prox.velocity << ")" << std::endl;
      std::cout << "- Load: "
                << Scale(PerchingArm::K_LOAD_JOINT_MA, raw_.prox.load)
                << " mA (raw: " << raw_.prox.load << ")" << std::endl;
      std::cout << "DISTAL MOTOR (PAN)" << std::endl;
      std::cout << "- Position: "
                << Scale(PerchingArm::K_POSITION_DEG, raw_.dist.position)
                << " deg (raw: " << raw_.dist.position << ")" << std::endl;
      std::cout << "- Velocity: "
                << Scale(PerchingArm::K_VELOCITY_RPM, raw_.dist.velocity)
                << " rpm (raw: " << raw_.dist.velocity << ")" << std::endl;
      std::cout << "- Load: "
                << Scale(PerchingArm::K_LOAD_JOINT_MA, raw_.dist.load)
                << " mA (raw: " << raw_.dist.load << ")" << std::endl;
      std::cout << "GRIPPER" << std::endl;
      std::cout << "- Adhesives: "
                << (raw_.grip.adhesive_engage ? "engaged" : "disengaged")
                << std::endl;
      std::cout << "- Wrist: "
                << (raw_.grip.wrist_lock ? "locked" : "unlocked")
                << std::endl;
      std::cout << "- Automatic mode: "
                << (raw_.grip.automatic_mode_enable ? "enabled" : "disabled")
                << std::endl;
      std::cout << "- File: "
                << (raw_.grip.file_is_open? "File is open" : "File is closed")
                << std::endl;
      std::cout << "- Exp idx: " << raw_.grip.exp_idx <<  std::endl;
      std::cout << "- Delay: " << raw_.grip.delay_ms << " [ms] " << std::endl;
      std::cout << "BOARD" << std::endl;
      std::cout << "- 5V current: "
                << Scale(PerchingArm::K_CURRENT_5V, raw_.current_5v)
                << " mA (raw: " << raw_.current_5v << ")" << std::endl;
      std::cout << "- 11V current "
                << Scale(PerchingArm::K_CURRENT_11V, raw_.current_11v)
                << " mA (raw: " << raw_.current_11v << ")" << std::endl;
      std::cout << "- Temperature: "
                << Scale(PerchingArm::K_TEMPERATURE_DEG, raw_.board_temp)
                << " deg (raw: " << raw_.board_temp << ")" << std::endl;
      std::cout << "- Loop time: "
                << Scale(PerchingArm::K_LOOP_TIME_MS, raw_.loop_time)
                << " ms (raw: " << raw_.loop_time << ")" << std::endl;
      return true;
    }
    // Set the proximal (tilt) position
    case 't': {
      std::cout << "Input proximal (tilt) position in degrees:" << std::endl;
      int16_t val;
      if (!InputSignedShort(val, PerchingArm::PROX_POS_MIN,
                            PerchingArm::PROX_POS_MAX))
        return true;
      ret = arm.SetProximalPosition(val);
      PrintResult(arm, ret);
      WaitForResultWithTimeout(WAITING_ON_PROX_POS, val);
      return true;
    }
    // Set the proximal (tilt) velocity
    case 'u': {
      std::cout << "Input proximal (tilt) velocity in RPM:" << std::endl;
      int16_t val;
      if (!InputSignedShort(val, PerchingArm::PROX_VEL_MIN,
                            PerchingArm::PROX_VEL_MAX))
        return true;
      ret = arm.SetProximalVelocity(val);
      PrintResult(arm, ret);
      return true;
    }
    // Set the distal (pan) position
    case 'p': {
      std::cout << "Input distal (pan) position in degrees:" << std::endl;
      int16_t val;
      if (!InputSignedShort(val, PerchingArm::DIST_POS_MIN,
                            PerchingArm::DIST_POS_MAX))
        return true;
      ret = arm.SetDistalPosition(val);
      PrintResult(arm, ret);
      WaitForResultWithTimeout(WAITING_ON_DIST_POS, val);
      return true;
    }
    // Set the distal (pan) velocity
    case 'q': {
      std::cout << "Input distal (pan) velocity in RPM:" << std::endl;
      int16_t val;
      if (!InputSignedShort(val, PerchingArm::DIST_VEL_MIN,
                            PerchingArm::DIST_VEL_MAX))
        return true;
      ret = arm.SetDistalVelocity(val);
      PrintResult(arm, ret);
      return true;
    }
    // Toggle the pan
    case 'P': {
      en_dist_ = !en_dist_;
      ret = arm.SetDistalEnabled();
      PrintResult(arm, ret);
      return true;
    }
    // Enable the pan motor
    case 'X': {
      arm.SetProximalEnabled();
      return true;
    }
    // Disable the pan motor
    case 'Y': {
      arm.SetProximalDisabled();
      return true;
    }
    // Enable the tilt motor
    case 'A': {
      arm.SetDistalEnabled();
      return true;
    }
    // Disable the tilt motor
    case 'B': {
      arm.SetDistalDisabled();
      return true;
    }
    // Open the gripper
    case 'O': {
      ret = arm.OpenGripper();
      return true;
    }
    // Close the gripper
    case 'C': {
      ret = arm.CloseGripper();
      return true;
    }
    // Engage the gripper
    case 'G': {
      ret = arm.EngageGripper();
      return true;
    }
    // Disengage the gripper
    case 'N': {
      ret = arm.DisengageGripper();
      return true;
    }
    // Lock the gripper
    case 'L': {
      arm.LockGripper();
      return true;
    }
    // Unlock the gripper
    case 'U': {
      arm.UnlockGripper();
      return true;
    }
    // Enable the gripper
    case 'E': {
      arm.EnableAutoGripper();
      return true;
    }
    // Disable the gripper
    case 'D': {
      arm.DisableAutoGripper();
      return true;
    }
    // Toggle auto gripper
    case 'T': {
      arm.ToggleAutoGripper();
      return true;
    }
    // Mark gripper
    case 'M': {
      uint16_t min = 0;
      uint16_t max = 65535;
      uint16_t IDX;
      if (!InputUnsignedShort(IDX, min, max)) {
        std::cout << "Invalid IDX value for MarkGripper()!" << std::endl;
        return false;
      }
      arm.MarkGripper(IDX);
      return true;
    }
    // Set delay for gripper
    case 'S': {
      int16_t min = 0;
      int16_t max = 32767;
      int16_t DL;
      if (!InputSignedShort(DL, min, max)) {
        std::cout << "Invalid DL value for SetDelayGripper(DL)!" << std::endl;
        return false;
      } else {
        std::cout << "Delay is " << DL << " milliseconds!" << std::endl;
      }
      arm.SetDelayGripper(DL);
      return true;
    }
    // Open experiment
    case 'F': {
      uint16_t min = 0;
      uint16_t max = 65535;
      uint16_t IDX;
      if (!InputUnsignedShort(IDX, min, max)) {
        std::cout << "Invalid IDX value for OpenExperimentGripper(IDX)!" << std::endl;
        return false;
      } else {
        std::cout << "IDX is " << IDX << "!" << std::endl;
      }
      arm.OpenExperimentGripper(static_cast<int16_t>(IDX));
      return true;
    }
    // Next record
    case 'R': {
      int16_t min = 0;
      int16_t max = 32767;
      int16_t SKIP;
      if (!InputSignedShort(SKIP, min, max)) {
        std::cout << "Invalid SKIP value for NextRecordGripper(SKIP)!" << std::endl;
        return false;
      } else {
        std::cout << "SKIP is " << SKIP << "!" << std::endl;
      }
      arm.NextRecordGripper(SKIP);
      return true;
    }
    // Seek record
    case 'K': {
      int16_t min = 0;
      int16_t max = 32767;
      int16_t RN;
      if (!InputSignedShort(RN, min, max)) {
        std::cout << "Invalid RN value for SeekRecordGripper(RN)!" << std::endl;
        return false;
      } else {
        std::cout << "RN is " << RN << "!" << std::endl;
      }
      arm.SeekRecordGripper(RN);
      return true;
    }
    // Close experiment
    case 'c': {
      arm.CloseExperimentGripper();
      return true;
    }
    // Read gripper status
    case 'a': {
      arm.Status();
      return true;
    }
    // Read SD record
    case 'l': {
      arm.Record();
      return true;
    }
    // Read TOF delay
    case 'n': {
      arm.PresentDelay();
      return true;
    }
    // Check experiment idx
    case 'o': {
      arm.ExperimentIdx();
      return true;
    }
    // Check error byte
    case 'e': {
      if (!raw_.grip.error_status) {
        std::cout << "No error currently!" << std::endl;
      }

      uint8_t err;
      if (raw_.grip.error_status && 0x80) {
        // Check if first bit is 1
        err = raw_.grip.error_status & 0x7F;
      } else {
        // If first bit = 0, ignore trailing bits
        err = 0x00;
      }
      err = raw_.grip.error_status & 0x7F;

      switch (err) {
        case perching_arm::ERR_RESULT:
          std::cout << "Result Fail" << std::endl;
        case perching_arm::ERR_INSTR:
          std::cout << "Instruction Fail" << std::endl;
        case perching_arm::ERR_CRC:
          std::cout << "CRC Error" << std::endl;
        case perching_arm::ERR_DATA_RANGE:
          std::cout << "Data Range Error" << std::endl;
        case perching_arm::ERR_DATA_LEN:
          std::cout << "Data Length Error" << std::endl;
        case perching_arm::ERR_DATA_LIM:
        std::cout << "Data Limit Error" << std::endl;
        case perching_arm::ERR_ACCESS:
          std::cout << "Access Error" << std::endl;
        case perching_arm::ERR_INSTR_READ:
          std::cout << "Instruction Read Error" << std::endl;
        case perching_arm::ERR_INSTR_WRITE:
          std::cout << "Instruction Write Error" << std::endl;
        case perching_arm::ERR_TOF_INIT:
          std::cout << "TOF Init Error" << std::endl;
        case perching_arm::ERR_TOF_READ:
          std::cout << "TOF Read Error" << std::endl;
        case perching_arm::ERR_SD_INIT:
          std::cout << "SD Init Error" << std::endl;
        case perching_arm::ERR_SD_OPEN:
          std::cout << "SD Open Error" << std::endl;
        case perching_arm::ERR_SD_WRITE:
          std::cout << "SD Write Error" << std::endl;
        case perching_arm::ERR_SD_READ:
          std::cout << "SD Read Error" << std::endl;
        default:
          std::cout << "Default error flag!" << std::endl;
          break;
      }
      return true;
    }
    // Perform a hard reset
    case 'h': {
      ret = arm.HardReset();
      PrintResult(arm, ret);
      return true;
    }
    // Perform a soft reset
    case 's': {
      ret = arm.SoftReset();
      PrintResult(arm, ret);
      return true;
    }
    // Send custom command
    case 'r': {
      uint16_t target, address;
      int16_t value;
      std::cout << "> Input target: " << std::endl;
      if (!InputUnsignedShort(target, 0, 10)) return true;
      std::cout << "> Input address: " << std::endl;
      if (!InputUnsignedShort(address, 0, 255)) return true;
      std::cout << "> Input signed integer value: " << std::endl;
      if (!InputSignedShort(value, SHRT_MIN, SHRT_MAX)) return true;
      ret = arm.SendCommand(target, address, value);
      PrintResult(arm, ret);
      return true;
    }
    default: {
      std::cerr << "Invalid selection" << std::endl;
      return true;
    }
  }
  return true;
}

}  // namespace perching_arm

// Main entry point for application
int main(int argc, char *argv[]) {
  // Gather some data from the command
  google::SetUsageMessage("Usage: rosrun perching_arm perching_arm_tool <opt>");
  google::SetVersionString("0.1.0");
  google::ParseCommandLineFlags(&argc, &argv, true);
  // If we specified a log file we should open it
  if (!FLAGS_log.empty()) {
    perching_arm::logfile_.open(FLAGS_log,
                                std::ofstream::out | std::ofstream::app);
    if (!perching_arm::logfile_.is_open()) {
      std::cerr << "Could not open the log file: " << FLAGS_log << std::endl;
      return 1;
    }
  }
  // Create the callback
  perching_arm::PerchingArmSleepMsCallback cb_sleep_ms =
      std::bind(perching_arm::SleepCallback, std::placeholders::_1);
  perching_arm::PerchingArmRawDataCallback cb_raw_data =
      std::bind(perching_arm::DataCallback, std::placeholders::_1);
  // Create the interface to the perching arm
  perching_arm::PerchingArm arm;
  perching_arm::PerchingArmResult ret =
      arm.Connect(FLAGS_port, FLAGS_baud, cb_sleep_ms, cb_raw_data);
  if (ret != perching_arm::RESULT_SUCCESS) {
    std::cerr << "Could not open the serial port: " << FLAGS_port << std::endl;
    return 1;
  }
  // Print a menu to start with
  perching_arm::PrintMenu();
  // Keep taking commands until termination request
  while (perching_arm::GetCommand(arm)) {
  }
  // Disconnect before shutdown
  arm.Disconnect();
  // Success
  return 0;
}
