// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2010-2012 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

#ifdef _WIN32
#include "boinc_win.h"
#include "win_util.h"
#else
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#endif

using std::string;

#if defined(_MSC_VER)
#define getcwd      _getcwd
#endif

#include "diagnostics.h"
#include "filesys.h"
#include "parse.h"
#include "str_util.h"
#include "str_replace.h"
#include "util.h"
#include "error_numbers.h"
#include "procinfo.h"
#include "network.h"
#include "boinc_api.h"
#include "floppyio.h"
#include "vboxwrapper.h"
#include "vbox.h"

static bool is_client_version_newer(APP_INIT_DATA& aid, int maj, int min, int rel) {
    if (maj < aid.major_version) return true;
    if (maj > aid.major_version) return false;
    if (min < aid.minor_version) return true;
    if (min > aid.minor_version) return false;
    if (rel < aid.release) return true;
    return false;
}

VBOX_VM::VBOX_VM() {
    virtualbox_home_directory.clear();
    virtualbox_install_directory.clear();
    virtualbox_version.clear();
    pFloppy = NULL;
    vm_master_name.clear();
    vm_master_description.clear();
    vm_name.clear();
    vm_cpu_count.clear();
    vm_disk_controller_type.clear();
    vm_disk_controller_model.clear();
    os_name.clear();
    memory_size_mb.clear();
    image_filename.clear();
    floppy_image_filename.clear();
    job_duration = 0.0;
    fraction_done_filename.clear();
    suspended = false;
    network_suspended = false;
    online = false;
    crashed = false;
    enable_cern_dataformat = false;
    enable_shared_directory = false;
    enable_floppyio = false;
    enable_remotedesktop = false;
    register_only = false;
    enable_network = false;
    pf_guest_port = 0;
    pf_host_port = 0;
    headless = true;
#ifdef _WIN32
    vm_pid_handle = 0;
    vboxsvc_handle = 0;
#else
    vm_pid = 0;
#endif

    // Initialize default values
    vm_disk_controller_type = "ide";
    vm_disk_controller_model = "PIIX4";
}

VBOX_VM::~VBOX_VM() {
    if (pFloppy) {
        delete pFloppy;
        pFloppy = NULL;
    }
#ifdef _WIN32
    if (vm_pid_handle) {
        CloseHandle(vm_pid_handle);
        vm_pid_handle = NULL;
    }
    if (vboxsvc_handle) {
        CloseHandle(vboxsvc_handle);
        vboxsvc_handle = NULL;
    }
#endif
}

int VBOX_VM::initialize() {
    int rc = 0;
    string old_path;
    string new_path;
    string command;
    string output;
    APP_INIT_DATA aid;
    bool force_sandbox = false;
    char buf[256];

    boinc_get_init_data_p(&aid);
    get_install_directory(virtualbox_install_directory);

    // Prep the environment so we can execute the vboxmanage application
    //
    // TODO: Fix for non-Windows environments if we ever find another platform
    // where vboxmanage is not already in the search path
#ifdef _WIN32
    if (!virtualbox_install_directory.empty())
    {
        old_path = getenv("PATH");
        new_path = virtualbox_install_directory + ";" + old_path;

        if (!SetEnvironmentVariable("PATH", const_cast<char*>(new_path.c_str()))) {
            fprintf(
                stderr,
                "%s Failed to modify the search path.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
        }
    }
#endif

    // Determine the VirtualBox home directory.  Overwrite as needed.
    //
    if (getenv("VBOX_USER_HOME")) {
        virtualbox_home_directory = getenv("VBOX_USER_HOME");
    } else {
        // If the override environment variable isn't specified then
        // it is based of the current users HOME directory.
#ifdef _WIN32
        virtualbox_home_directory = getenv("USERPROFILE");
#else
        virtualbox_home_directory = getenv("HOME");
#endif
        virtualbox_home_directory += "/.VirtualBox";
    }

    // On *nix style systems, VirtualBox expects that there is a home directory specified
    // by environment variable.  When it doesn't exist it attempts to store logging information
    // in root's home directory.  Bad things happen if the process isn't owned by root.
    //
    // if the HOME environment variable is missing force VirtualBox to use a directory it
    // has a reasonable chance of writing log files too.
#ifndef _WIN32
    if (NULL == getenv("HOME")) {
        force_sandbox = true;
    }
#endif

    // Set the location in which the VirtualBox Configuration files can be
    // stored for this instance.
    if (aid.using_sandbox || force_sandbox) {
        virtualbox_home_directory = aid.project_dir;
        virtualbox_home_directory += "/../virtualbox";

        if (!boinc_file_exists(virtualbox_home_directory.c_str())) boinc_mkdir(virtualbox_home_directory.c_str());

#ifdef _WIN32
        if (!SetEnvironmentVariable("VBOX_USER_HOME", const_cast<char*>(virtualbox_home_directory.c_str()))) {
            fprintf(
                stderr,
                "%s Failed to modify the search path.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
        }

        // Launch vboxsvc before any vboxmanage command can be executed.
        launch_vboxsvc();

#else
        // putenv does not copy its input buffer, so we must use setenv
        if (setenv("VBOX_USER_HOME", const_cast<char*>(virtualbox_home_directory.c_str()), 1)) {
            fprintf(
                stderr,
                "%s Failed to modify the VBOX_USER_HOME path.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
        }
#endif
    }

    // Record the VirtualBox version information for later use.
    command = "--version ";
    rc = vbm_popen(command, output, "version check");

    // Remove \r or \n from the output spew
    string::iterator iter = output.begin();
    while (iter != output.end()) {
        if (*iter == '\r' || *iter == '\n') {
            iter = output.erase(iter);
        } else {
            ++iter;
        }
    }

    virtualbox_version = "VirtualBox " + output;

    return rc;
}

int VBOX_VM::run(double elapsed_time) {
    int retval;

    if (!is_registered()) {
        if (is_hdd_registered()) {
            // Handle the case where a previous instance of the same projects VM
            // was already initialized for the current slot directory but aborted
            // while the task was suspended and unloaded from memory.
            retval = deregister_stale_vm();
            if (retval) return retval;
        }
        retval = register_vm();
        if (retval) return retval;
    }

    // The user has requested that we exit after registering the VM, so return an
    // error to stop further processing.
    if (register_only) return ERR_FOPEN;

    // If we are restarting an already registered VM, then the vm_name variable
    // will be empty right now, so populate it with the master name so all of the
    // various other functions will work.
    vm_name = vm_master_name;

    // Check to see if the VM is already in a running state, if so, poweroff.
    poll(false);
    if (online) {
        poweroff();
    }

    // If our last checkpoint time is greater than 0, restore from the previously
    // saved snapshot
    if (elapsed_time) {
        restoresnapshot();
    }

    // Start the VM
    retval = start();
    if (retval) return retval;

    return 0;
}

int VBOX_VM::start() {
    string command;
    string output;
    double timeout;
    char buf[256];
    int retval;

    fprintf(
        stderr,
        "%s Starting VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command = "startvm \"" + vm_name + "\"";
    if (headless) {
        command += " --type headless";
    }
    retval = vbm_popen(command, output, "start VM");
    if (retval) return retval;

    // Wait for up to 5 minutes for the VM to switch states.  A system
    // under load can take a while.  Since the poll function can wait for up
    // to 45 seconds to execute a command we need to make this time based instead
    // of interation based.
    timeout = dtime() + 300;
    do {
        poll(false);
        if (online) break;
        boinc_sleep(1.0);
    } while (timeout >= dtime());

    if (online) {
        fprintf(
            stderr,
            "%s Successfully started VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        retval = BOINC_SUCCESS;
    } else {
        fprintf(
            stderr,
            "%s VM did not start within 5 minutes, aborting job.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        retval = ERR_EXEC;
    }

    return retval;
}

int VBOX_VM::stop() {
    string command;
    string output;
    char buf[256];
    int retval = 0;

    fprintf(
        stderr,
        "%s Stopping VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    if (online) {
        command = "controlvm \"" + vm_name + "\" savestate";
        retval = vbm_popen(command, output, "stop VM", true, false);

        poll(false);

        if (!online) {
            fprintf(
                stderr,
                "%s Successfully stopped VM.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
            retval = BOINC_SUCCESS;
        } else {
            fprintf(
                stderr,
                "%s VM did not stop when requested.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
            retval = ERR_EXEC;
        }
    }

    return retval;
}

int VBOX_VM::poweroff() {
    string command;
    string output;
    char buf[256];
    int retval = 0;

    fprintf(
        stderr,
        "%s Powering off VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    if (online) {
        command = "controlvm \"" + vm_name + "\" poweroff";
        retval = vbm_popen(command, output, "poweroff VM", true, false);

        poll(false);

        if (!online) {
            fprintf(
                stderr,
                "%s Successfully powered off VM.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
            retval = BOINC_SUCCESS;
        } else {
            fprintf(
                stderr,
                "%s VM did not power off when requested.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
            retval = ERR_EXEC;
        }
    }

    return retval;
}

int VBOX_VM::pause() {
    string command;
    string output;
    int retval;

    // Restore the process priority back to the default process priority
    // to speed up the last minute maintenance tasks before the VirtualBox
    // VM goes to sleep
    //
    reset_vm_process_priority();

    command = "controlvm \"" + vm_name + "\" pause";
    retval = vbm_popen(command, output, "pause VM");
    if (retval) return retval;
    suspended = true;
    return 0;
}

int VBOX_VM::resume() {
    string command;
    string output;
    int retval;

    // Set the process priority back to the lowest level before resuming
    // execution
    //
    lower_vm_process_priority();

    command = "controlvm \"" + vm_name + "\" resume";
    retval = vbm_popen(command, output, "resume VM");
    if (retval) return retval;
    suspended = false;
    return 0;
}

int VBOX_VM::createsnapshot(double elapsed_time) {
    string command;
    string output;
    char buf[256];
    int retval;

    fprintf(
        stderr,
        "%s Creating new snapshot for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );

    // Pause VM - Try and avoid the live snapshot and trigger an online
    // snapshot instead.
    pause();

    // Create new snapshot
    sprintf(buf, "%d", (int)elapsed_time);
    command = "snapshot \"" + vm_name + "\" ";
    command += "take boinc_";
    command += buf;
    retval = vbm_popen(command, output, "create new snapshot", true, true, 0);
    if (retval) return retval;

    // Resume VM
    resume();

    // Set the suspended flag back to false before deleting the stale
    // snapshot
    poll(false);

    // Delete stale snapshot(s), if one exists
    cleanupsnapshots(false);

    fprintf(
        stderr,
        "%s Checkpoint completed.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );

    return 0;
}

int VBOX_VM::cleanupsnapshots(bool delete_active) {
    string command;
    string output;
    string line;
    string uuid;
    size_t eol_pos;
    size_t eol_prev_pos;
    size_t uuid_start;
    size_t uuid_end;
    char buf[256];
    int retval;


    // Enumerate snapshot(s)
    command = "snapshot \"" + vm_name + "\" ";
    command += "list ";
    retval = vbm_popen(command, output, "enumerate snapshot(s)");
    if (retval) return retval;

    // Output should look a little like this:
    //   Name: Snapshot 2 (UUID: 1751e9a6-49e7-4dcc-ab23-08428b665ddf)
    //      Name: Snapshot 3 (UUID: 92fa8b35-873a-4197-9d54-7b6b746b2c58)
    //         Name: Snapshot 4 (UUID: c049023a-5132-45d5-987d-a9cfadb09664) *
    //
    eol_prev_pos = 0;
    eol_pos = output.find("\n");
    while (eol_pos != string::npos) {
        line = output.substr(eol_prev_pos, eol_pos - eol_prev_pos);

        // This VM does not yet have any snapshots
        if (line.find("does not have any snapshots") != string::npos) break;

        // The * signifies that it is the active snapshot and one we do not want to delete
        if (!delete_active && (line.find("*") != string::npos)) break;

        uuid_start = line.find("(UUID: ");
        if (uuid_start != string::npos) {
            // We can parse the virtual machine ID from the line
            uuid_start += 7;
            uuid_end = line.find(")", uuid_start);
            uuid = line.substr(uuid_start, uuid_end - uuid_start);

            fprintf(
                stderr,
                "%s Deleting stale snapshot.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );

            // Delete stale snapshot, if one exists
            command = "snapshot \"" + vm_name + "\" ";
            command += "delete \"";
            command += uuid;
            command += "\" ";
            
            vbm_popen(command, output, "delete stale snapshot", true, false, 0);
        }

        eol_prev_pos = eol_pos + 1;
        eol_pos = output.find("\n", eol_prev_pos);
    }

    return 0;
}

int VBOX_VM::restoresnapshot() {
    string command;
    string output;
    char buf[256];
    int retval;

    fprintf(
        stderr,
        "%s Restore from previously saved snapshot.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );

    command = "snapshot \"" + vm_name + "\" ";
    command += "restorecurrent ";
    retval = vbm_popen(command, output, "restore current snapshot");
    if (retval) return retval;

    fprintf(
        stderr,
        "%s Restore completed.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );

    return 0;
}

void VBOX_VM::cleanup() {
    poweroff();
    deregister_vm(true);

    // Give time enough for external processes to finish the cleanup process
    boinc_sleep(5.0);
}

void VBOX_VM::poll(bool log_state) {
    char buf[256];
    string command;
    string output;
    string vmstate;
    size_t vmstate_start;
    size_t vmstate_end;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--machinereadable ";

    if (vbm_popen(command, output, "VM state", false, false) == 0) {
        vmstate_start = output.find("VMState=\"");
        if (vmstate_start != string::npos) {
            vmstate_start += 9;
            vmstate_end = output.find("\"", vmstate_start);
            vmstate = output.substr(vmstate_start, vmstate_end - vmstate_start);

            // VirtualBox Documentation suggests that that a VM is running when its
            // machine state is between MachineState_FirstOnline and MachineState_LastOnline
            // which as of this writing is 5 and 17.
            //
            // VboxManage's source shows more than that though:
            // see: http://www.virtualbox.org/browser/trunk/src/VBox/Frontends/VBoxManage/VBoxManageInfo.cpp
            //
            // So for now, go with what VboxManage is reporting.
            //
            if (vmstate == "running") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "paused") {
                online = true;
                suspended = true;
                crashed = false;
            } else if (vmstate == "starting") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "stopping") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "saving") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "restoring") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "livesnapshotting") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "deletingsnapshotlive") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "deletingsnapshotlivepaused") {
                online = true;
                suspended = false;
                crashed = false;
            } else if (vmstate == "aborted") {
                online = false;
                suspended = false;
                crashed = true;
            } else if (vmstate == "gurumeditation") {
                online = false;
                suspended = false;
                crashed = true;
            } else {
                online = false;
                suspended = false;
                crashed = false;
                if (log_state) {
                    fprintf(
                        stderr,
                        "%s VM is no longer is a running state. It is in '%s'.\n",
                        vboxwrapper_msg_prefix(buf, sizeof(buf)),
                        vmstate.c_str()
                    );
                }
            }
        }
    }
}

// Attempt to detect any condition that would prevent VirtualBox from running a VM properly, like:
// 1. The DCOM service not being started on Windows
// 2. Vboxmanage not being able to communicate with vboxsvc for some reason
// 3. VirtualBox driver not loaded for the current Linux kernel.
//
// Luckly both of the above conditions can be detected by attempting to detect the host information
// via vboxmanage and it is cross platform.
//
bool VBOX_VM::is_system_ready(std::string& message) {
    string command;
    string output;
    bool rc = true;

    command  = "list hostinfo ";
    if (vbm_popen(command, output, "host info") == 0) {

        if (output.find("Processor count:") == string::npos) {
            message = "Communication with VM Hypervisor failed.";
            rc = false;
        }

        if (output.find("WARNING: The vboxdrv kernel module is not loaded.") != string::npos) {
            message = "Please update/recompile VirtualBox kernel drivers.";
            rc = false;
        }

    }

    return rc;
}

bool VBOX_VM::is_registered() {
    string command;
    string output;

    command  = "showvminfo \"" + vm_master_name + "\" ";
    command += "--machinereadable ";

    if (vbm_popen(command, output, "registration", false, false) == 0) {
        if (output.find("VBOX_E_OBJECT_NOT_FOUND") == string::npos) {
            // Error message not found in text
            return true;
        }
    }
    return false;
}

bool VBOX_VM::is_hdd_registered() {
    string command;
    string output;
    string virtual_machine_root_dir;

    get_slot_directory(virtual_machine_root_dir);

    command = "showhdinfo \"" + virtual_machine_root_dir + "/" + image_filename + "\" ";

    if (vbm_popen(command, output, "hdd registration", false, false) == 0) {
        if ((output.find("VBOX_E_FILE_ERROR") == string::npos) && 
            (output.find("VBOX_E_OBJECT_NOT_FOUND") == string::npos) &&
            (output.find("does not match the value") == string::npos)
        ) {
            // Error message not found in text
            return true;
        }
    }
    return false;
}

bool VBOX_VM::is_extpack_installed() {
    string command;
    string output;

    command = "list extpacks";

    if (vbm_popen(command, output, "extpack detection", false, false) == 0) {
        if ((output.find("Oracle VM VirtualBox Extension Pack") != string::npos) && (output.find("VBoxVRDP") != string::npos)) {
            return true;
        }
    }
    return false;
}

int VBOX_VM::register_vm() {
    string command;
    string output;
    string virtual_machine_slot_directory;
    APP_INIT_DATA aid;
    char buf[256];
    int retval;

    boinc_get_init_data_p(&aid);
    get_slot_directory(virtual_machine_slot_directory);


    // Reset VM name in case it was changed while deregistering a stale VM
    //
    vm_name = vm_master_name;


    fprintf(
        stderr,
        "%s Registering VM. (%s) \n",
        vboxwrapper_msg_prefix(buf, sizeof(buf)),
        vm_name.c_str()
    );


    // Create and register the VM
    //
    command  = "createvm ";
    command += "--name \"" + vm_name + "\" ";
    command += "--basefolder \"" + virtual_machine_slot_directory + "\" ";
    command += "--ostype \"" + os_name + "\" ";
    command += "--register";
    
    retval = vbm_popen(command, output, "register");
    if (retval) return retval;

    // Tweak the VM's Description
    //
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--description \"" + vm_master_description + "\" ";

    vbm_popen(command, output, "modifydescription", false, false);

    // Tweak the VM's CPU Count
    //
    fprintf(
        stderr,
        "%s Setting CPU Count for VM. (%s)\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf)),
        vm_cpu_count.c_str()
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--cpus " + vm_cpu_count + " ";

    retval = vbm_popen(command, output, "modifycpu");
    if (retval) return retval;

    // Tweak the VM's Memory Size
    //
    fprintf(
        stderr,
        "%s Setting Memory Size for VM. (%sMB)\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf)),
        memory_size_mb.c_str()
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--memory " + memory_size_mb + " ";

    retval = vbm_popen(command, output, "modifymem");
    if (retval) return retval;

    // Tweak the VM's Chipset Options
    //
    fprintf(
        stderr,
        "%s Setting Chipset Options for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--acpi on ";
    command += "--ioapic on ";

    retval = vbm_popen(command, output, "modifychipset");
    if (retval) return retval;

    // Tweak the VM's Boot Options
    //
    fprintf(
        stderr,
        "%s Setting Boot Options for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--boot1 disk ";
    command += "--boot2 none ";
    command += "--boot3 none ";
    command += "--boot4 none ";

    retval = vbm_popen(command, output, "modifyboot");
    if (retval) return retval;

    // Tweak the VM's Network Configuration
    //
    fprintf(
        stderr,
        "%s Setting Network Configuration for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--nic1 nat ";
    command += "--natdnsproxy1 on ";
    command += "--cableconnected1 off ";

    retval = vbm_popen(command, output, "modifynetwork");
    if (retval) return retval;

    // Tweak the VM's USB Configuration
    //
    fprintf(
        stderr,
        "%s Disabling USB Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--usb off ";

    vbm_popen(command, output, "modifyusb", false, false);

    // Tweak the VM's COM Port Support
    //
    fprintf(
        stderr,
        "%s Disabling COM Port Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--uart1 off ";
    command += "--uart2 off ";

    vbm_popen(command, output, "modifycom", false, false);

    // Tweak the VM's LPT Port Support
    //
    fprintf(
        stderr,
        "%s Disabling LPT Port Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--lpt1 off ";
    command += "--lpt2 off ";

    vbm_popen(command, output, "modifylpt", false, false);

    // Tweak the VM's Audio Support
    //
    fprintf(
        stderr,
        "%s Disabling Audio Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--audio none ";

    vbm_popen(command, output, "modifyaudio", false, false);

    // Tweak the VM's Clipboard Support
    //
    fprintf(
        stderr,
        "%s Disabling Clipboard Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--clipboard disabled ";

    vbm_popen(command, output, "modifyclipboard", false, false);

    // Tweak the VM's Drag & Drop Support
    //
    fprintf(
        stderr,
        "%s Disabling Drag and Drop Support for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--draganddrop disabled ";

    vbm_popen(command, output, "modifydragdrop", false, false);

    // Only perform hardware acceleration check on 32-bit VM types, 64-bit VM types require it.
    //
    if (os_name.find("_64") == std::string::npos) {

        // Check to see if the processor supports hardware acceleration for virtualization
        // If it doesn't, disable the use of it in VirtualBox. Multi-core jobs require hardware
        // acceleration and actually override this setting.
        //
        bool disable_acceleration = false;

        if (!strstr(aid.host_info.p_features, "vmx") && !strstr(aid.host_info.p_features, "svm")) {
            fprintf(
                stderr,
                "%s Hardware acceleration CPU extensions not detected. Disabling VirtualBox hardware acceleration support.\n",
                boinc_msg_prefix(buf, sizeof(buf))
            );
            disable_acceleration = true;
        }
        if (strstr(aid.host_info.p_features, "hypervisor")) {
            fprintf(
                stderr,
                "%s Running under Hypervisor. Disabling VirtualBox hardware acceleration support.\n",
                boinc_msg_prefix(buf, sizeof(buf))
            );
            disable_acceleration = true;
        }
        if (is_client_version_newer(aid, 7, 2, 16)) {
            if (aid.vm_extensions_disabled) {
                fprintf(
                    stderr,
                    "%s Hardware acceleration failed with previous execution. Disabling VirtualBox hardware acceleration support.\n",
                    boinc_msg_prefix(buf, sizeof(buf))
                );
                disable_acceleration = true;
            }
        } else {
            if (vm_cpu_count == "1") {
                // Keep this around for older clients.  Removing this for older clients might
                // lead to a machine that will only return crashed VM reports.
                disable_acceleration = true;
            }
        }

        if (disable_acceleration) {
            fprintf(
                stderr,
                "%s Disabling hardware acceleration support for virtualization.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
            command  = "modifyvm \"" + vm_name + "\" ";
            command += "--hwvirtex off ";

            retval = vbm_popen(command, output, "VT-x/AMD-V support");
            if (retval) return retval;
        }

    }

    // Add storage controller to VM
    // See: http://www.virtualbox.org/manual/ch08.html#vboxmanage-storagectl
    // See: http://www.virtualbox.org/manual/ch05.html#iocaching
    //
    fprintf(
        stderr,
        "%s Adding storage controller to VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "storagectl \"" + vm_name + "\" ";
    command += "--name \"Hard Disk Controller\" ";
    command += "--add \"" + vm_disk_controller_type + "\" ";
    command += "--controller \"" + vm_disk_controller_model + "\" ";
    command += "--hostiocache off ";
    if ((vm_disk_controller_type == "sata") || (vm_disk_controller_type == "SATA")) {
        command += "--sataportcount 1 ";
    }

    retval = vbm_popen(command, output, "add storage controller (fixed disk)");
    if (retval) return retval;

    // Add storage controller for a floppy device if desired
    //
    if (enable_floppyio) {
        command  = "storagectl \"" + vm_name + "\" ";
        command += "--name \"Floppy Controller\" ";
        command += "--add floppy ";

        retval = vbm_popen(command, output, "add storage controller (floppy)");
        if (retval) return retval;
    }

    // Adding virtual hard drive to VM
    //
    fprintf(
        stderr,
        "%s Adding virtual disk drive to VM. (%s)\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf)),
		image_filename.c_str()
    );
    command  = "storageattach \"" + vm_name + "\" ";
    command += "--storagectl \"Hard Disk Controller\" ";
    command += "--port 0 ";
    command += "--device 0 ";
    command += "--type hdd ";
    command += "--setuuid \"\" ";
    command += "--medium \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";

    retval = vbm_popen(command, output, "storage attach (fixed disk)");
    if (retval) return retval;

    // Adding virtual floppy disk drive to VM
    //
    if (enable_floppyio) {

        // Put in place the FloppyIO abstraction
        //
        // NOTE: This creates the floppy.img file at runtime for use by the VM.
        //
        pFloppy = new FloppyIO(floppy_image_filename.c_str());
        if (!pFloppy->ready()) {
            fprintf(
                stderr,
                "%s Creating virtual floppy image failed.\n"
                "%s Error Code '%d' Error Message '%s'\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf)),
                vboxwrapper_msg_prefix(buf, sizeof(buf)),
                pFloppy->error,
                pFloppy->errorStr.c_str()
            );
            return ERR_FWRITE;
        }

        fprintf(
            stderr,
            "%s Adding virtual floppy disk drive to VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        command  = "storageattach \"" + vm_name + "\" ";
        command += "--storagectl \"Floppy Controller\" ";
        command += "--port 0 ";
        command += "--device 0 ";
        command += "--medium \"" + virtual_machine_slot_directory + "/" + floppy_image_filename + "\" ";

        retval = vbm_popen(command, output, "storage attach (floppy disk)");
        if (retval) return retval;

    }

    // Enable the network adapter if a network connection is required.
    //
    if (enable_network) {
        set_network_access(true);

        // If the VM wants to open up a port through the VirtualBox virtual
        // network firewall/nat do that here.
        //
        if (pf_guest_port) {
            if (!pf_host_port) {
                retval = get_port_forwarding_port();
                if (retval) return retval;
            }

            fprintf(
                stderr,
                "%s Enabling VM firewall rules.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );

            // Add new firewall rule
            //
            sprintf(buf, "vboxwrapper,tcp,127.0.0.1,%d,,%d", pf_host_port, pf_guest_port);
            command  = "modifyvm \"" + vm_name + "\" ";
            command += "--natpf1 \"" + string(buf) + "\" ";

            retval = vbm_popen(command, output, "add updated port forwarding rule");
            if(retval) return retval;
        }
    }

    // If the VM wants to enable remote desktop for the VM do it here
    //
    if (enable_remotedesktop) {
        fprintf(
            stderr,
            "%s Enabling remote desktop for VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        if (!is_extpack_installed()) {
            fprintf(
                stderr,
                "%s Required extension pack not installed, remote desktop not enabled.\n",
                vboxwrapper_msg_prefix(buf, sizeof(buf))
            );
        } else {
            retval = get_remote_desktop_port();
            if (retval) return retval;

            sprintf(buf, "%d", rd_host_port);
            command  = "modifyvm \"" + vm_name + "\" ";
            command += "--vrde on ";
            command += "--vrdeextpack default ";
            command += "--vrdeauthlibrary default ";
            command += "--vrdeauthtype null ";
            command += "--vrdeport " + string(buf) + " ";

            retval = vbm_popen(command, output, "remote desktop");
            if(retval) return retval;
        }
    }

    // Enable the shared folder if a shared folder is specified.
    //
    if (enable_shared_directory) {
        fprintf(
            stderr,
            "%s Enabling shared directory for VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        command  = "sharedfolder add \"" + vm_name + "\" ";
        command += "--name \"shared\" ";
        command += "--hostpath \"" + virtual_machine_slot_directory + "/shared\"";

        retval = vbm_popen(command, output, "enable shared dir");
        if (retval) return retval;
    }

    return 0;
}

int VBOX_VM::deregister_vm(bool delete_media) {
    string command;
    string output;
    string virtual_machine_slot_directory;
    char buf[256];

    get_slot_directory(virtual_machine_slot_directory);

    fprintf(
        stderr,
        "%s Deregistering VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );


    // Cleanup any left-over snapshots
    //
    cleanupsnapshots(true);

    // Delete its storage controller(s)
    //
    fprintf(
        stderr,
        "%s Removing storage controller(s) from VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "storagectl \"" + vm_name + "\" ";
    command += "--name \"IDE Controller\" ";
    command += "--remove ";

    vbm_popen(command, output, "deregister storage controller (fixed disk)", false, false);

    if (enable_floppyio) {
        command  = "storagectl \"" + vm_name + "\" ";
        command += "--name \"Floppy Controller\" ";
        command += "--remove ";

        vbm_popen(command, output, "deregister storage controller (floppy disk)", false, false);
    }

    // Next, delete VM
    //
    fprintf(
        stderr,
        "%s Removing VM from VirtualBox.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "unregistervm \"" + vm_name + "\" ";
    command += "--delete ";

    vbm_popen(command, output, "delete VM", false, false);

    // Lastly delete medium(s) from Virtual Box Media Registry
    //
    fprintf(
        stderr,
        "%s Removing virtual disk drive from VirtualBox.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    command  = "closemedium disk \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";
    if (delete_media) {
        command += "--delete ";
    }

    vbm_popen(command, output, "remove virtual disk", false, false);

    if (enable_floppyio) {
        fprintf(
            stderr,
            "%s Removing virtual floppy disk from VirtualBox.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        command  = "closemedium floppy \"" + virtual_machine_slot_directory + "/" + floppy_image_filename + "\" ";
        if (delete_media) {
            command += "--delete ";
        }

        vbm_popen(command, output, "remove virtual floppy disk", false, false);
    }

    return 0;
}

int VBOX_VM::deregister_stale_vm() {
    string command;
    string output;
    string virtual_machine_slot_directory;
    size_t uuid_start;
    size_t uuid_end;
    int retval;

    get_slot_directory(virtual_machine_slot_directory);

    // We need to determine what the name or uuid is of the previous VM which owns
    // this virtual disk
    //
    command  = "showhdinfo \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";

    retval = vbm_popen(command, output, "get HDD info");
    if (retval) return retval;

    // Output should look a little like this:
    //   UUID:                 c119acaf-636c-41f6-86c9-38e639a31339
    //   Accessible:           yes
    //   Logical size:         10240 MBytes
    //   Current size on disk: 0 MBytes
    //   Type:                 normal (base)
    //   Storage format:       VDI
    //   Format variant:       dynamic default
    //   In use by VMs:        test2 (UUID: 000ab2be-1254-4c6a-9fdc-1536a478f601)
    //   Location:             C:\Users\romw\VirtualBox VMs\test2\test2.vdi
    //
    uuid_start = output.find("(UUID: ");
    if (uuid_start != string::npos) {
        // We can parse the virtual machine ID from the output
        uuid_start += 7;
        uuid_end = output.find(")", uuid_start);
        vm_name = output.substr(uuid_start, uuid_end - uuid_start);

        // Deregister stale VM by UUID
        return deregister_vm(false);
    } else {
        // Did the user delete the VM in VirtualBox and not the medium?  If so,
        // just remove the medium.
        command  = "closemedium disk \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";
        vbm_popen(command, output, "remove virtual disk", false);
        if (enable_floppyio) {
            command  = "closemedium floppy \"" + virtual_machine_slot_directory + "/" + floppy_image_filename + "\" ";
            vbm_popen(command, output, "remove virtual floppy disk", false);
        }
    }
    return 0;
}

int VBOX_VM::get_install_directory(string& install_directory ) {
#ifdef _WIN32
    LONG    lReturnValue;
    HKEY    hkSetupHive;
    LPTSTR  lpszRegistryValue = NULL;
    DWORD   dwSize = 0;

    // change the current directory to the boinc data directory if it exists
    lReturnValue = RegOpenKeyEx(
        HKEY_LOCAL_MACHINE, 
        _T("SOFTWARE\\Oracle\\VirtualBox"),  
        0, 
        KEY_READ,
        &hkSetupHive
    );
    if (lReturnValue == ERROR_SUCCESS) {
        // How large does our buffer need to be?
        lReturnValue = RegQueryValueEx(
            hkSetupHive,
            _T("InstallDir"),
            NULL,
            NULL,
            NULL,
            &dwSize
        );
        if (lReturnValue != ERROR_FILE_NOT_FOUND) {
            // Allocate the buffer space.
            lpszRegistryValue = (LPTSTR) malloc(dwSize);
            (*lpszRegistryValue) = NULL;

            // Now get the data
            lReturnValue = RegQueryValueEx( 
                hkSetupHive,
                _T("InstallDir"),
                NULL,
                NULL,
                (LPBYTE)lpszRegistryValue,
                &dwSize
            );

            install_directory = lpszRegistryValue;
        }
    }

    if (hkSetupHive) RegCloseKey(hkSetupHive);
    if (lpszRegistryValue) free(lpszRegistryValue);
    if (install_directory.empty()) {
        return 1;
    }
    return 0;
#else
    install_directory = "";
    return 0;
#endif
}

// Returns the current directory in which the executable resides.
//
int VBOX_VM::get_slot_directory(string& dir) {
    char slot_dir[256];

    getcwd(slot_dir, sizeof(slot_dir));
    dir = slot_dir;

    if (!dir.empty()) {
        return 1;
    }
    return 0;
}

int VBOX_VM::get_network_bytes_sent(double& sent) {
    string command;
    string output;
    string counter_value;
    size_t counter_start;
    size_t counter_end;
    int retval;

    command  = "debugvm \"" + vm_name + "\" ";
    command += "statistics --pattern \"/Devices/*/TransmitBytes\" ";

    retval = vbm_popen(command, output, "get bytes sent");
    if (retval) return retval;

    // Output should look like this:
    // <?xml version="1.0" encoding="UTF-8" standalone="no"?>
    // <Statistics>
    // <Counter c="397229" unit="bytes" name="/Devices/PCNet0/TransmitBytes"/>
    // <Counter c="256" unit="bytes" name="/Devices/PCNet1/TransmitBytes"/>
    // </Statistics>

    // add up the counter(s)
    //
    sent = 0;
    counter_start = output.find("c=\"");
    while (counter_start != string::npos) {
        counter_start += 3;
        counter_end = output.find("\"", counter_start);
        counter_value = output.substr(counter_start, counter_end - counter_start);
        sent += atof(counter_value.c_str());
        counter_start = output.find("c=\"", counter_start);
    }
    return 0;
}

int VBOX_VM::get_network_bytes_received(double& received) {
    string command;
    string output;
    string counter_value;
    size_t counter_start;
    size_t counter_end;
    int retval;

    command  = "debugvm \"" + vm_name + "\" ";
    command += "statistics --pattern \"/Devices/*/ReceiveBytes\" ";

    retval = vbm_popen(command, output, "get bytes received");
    if (retval) return retval;

    // Output should look like this:
    // <?xml version="1.0" encoding="UTF-8" standalone="no"?>
    // <Statistics>
    // <Counter c="9423150" unit="bytes" name="/Devices/PCNet0/ReceiveBytes"/>
    // <Counter c="256" unit="bytes" name="/Devices/PCNet1/ReceiveBytes"/>
    // </Statistics>

    // add up the counter(s)
    //
    received = 0;
    counter_start = output.find("c=\"");
    while (counter_start != string::npos) {
        counter_start += 3;
        counter_end = output.find("\"", counter_start);
        counter_value = output.substr(counter_start, counter_end - counter_start);
        received += atof(counter_value.c_str());
        counter_start = output.find("c=\"", counter_start);
    }

    return 0;
}

int VBOX_VM::get_system_log(string& log) {
    string slot_directory;
    string virtualbox_system_log_src;
    string virtualbox_system_log_dst;
    string::iterator iter;
    char buf[256];
    int retval = 0;

    // Where should we copy temp files to?
    get_slot_directory(slot_directory);

    // Locate and read log file
    virtualbox_system_log_src = virtualbox_home_directory + "/VBoxSVC.log";
    virtualbox_system_log_dst = slot_directory + "/VBoxSVC.log";

    if (boinc_file_exists(virtualbox_system_log_src.c_str())) {
        // Skip having to deal with various forms of file locks by just making a temp
        // copy of the log file.
        boinc_copy(virtualbox_system_log_src.c_str(), virtualbox_system_log_dst.c_str());

        // Keep only the last 16k if it is larger than that.
        read_file_string(virtualbox_system_log_dst.c_str(), log, 16384, true);

#ifdef _WIN32
        // Remove \r from the log spew
        iter = log.begin();
        while (iter != log.end()) {
            if (*iter == '\r') {
                iter = log.erase(iter);
            } else {
                ++iter;
            }
        }
#endif

        if (log.size() >= 16384) {
            // Look for the next whole line of text.
            iter = log.begin();
            while (iter != log.end()) {
                if (*iter == '\n') {
                    log.erase(iter);
                    break;
                }
                iter = log.erase(iter);
            }
        }
    } else {
        fprintf(
            stderr,
            "%s Could not find the Hypervisor System Log at '%s'.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf)),
            virtualbox_system_log_src.c_str()
        );
        retval = ERR_NOT_FOUND;
    }

    return retval;
}

int VBOX_VM::get_vm_log(string& log) {
    string command;
    string output;
    string::iterator iter;
    int retval;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--log 0 ";

    retval = vbm_popen(command, output, "get vm log", false, false);
    if (retval) {
        // Check to see if this error code is really an error.  Every once and awhile
        // vboxmanage will return a non-zero exit code even though it properly
        // dumps the vm log to stdout
        //
        if (output.find("Process ID: ") == string::npos) {
            return retval;
        }
    }

    // Keep only the last 16k if it is larger than that.
    size_t size = output.size();
    if (size > 16384) {
        log = output.substr(size - 16384, size);
        if (log.size() >= 16384) {
            // Look for the next whole line of text.
            iter = log.begin();
            while (iter != log.end()) {
                if (*iter == '\n') {
                    log.erase(iter);
                    break;
                }
                iter = log.erase(iter);
            }
        }
    } else {
        log = output;
    }

    return 0;
}

int VBOX_VM::get_vm_exit_code(unsigned long& exit_code) {
#ifndef _WIN32
    int ec = 0;
    waitpid(vm_pid, &ec, WNOHANG);
    exit_code = ec;
#else
    GetExitCodeProcess(vm_pid_handle, &exit_code);
#endif
    return 0;
}

int VBOX_VM::get_vm_process_id(int& process_id) {
    string command;
    string output;
    string pid;
    size_t pid_start;
    size_t pid_end;
    int retval;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--log 0 ";

    retval = vbm_popen(command, output, "get process ID");
    if (retval) return retval;

    // Output should look like this:
    // VirtualBox 4.1.0 r73009 win.amd64 (Jul 19 2011 13:05:53) release log
    // 00:00:06.008 Log opened 2011-09-01T23:00:59.829170900Z
    // 00:00:06.008 OS Product: Windows 7
    // 00:00:06.009 OS Release: 6.1.7601
    // 00:00:06.009 OS Service Pack: 1
    // 00:00:06.015 Host RAM: 4094MB RAM, available: 876MB
    // 00:00:06.015 Executable: C:\Program Files\Oracle\VirtualBox\VirtualBox.exe
    // 00:00:06.015 Process ID: 6128
    // 00:00:06.015 Package type: WINDOWS_64BITS_GENERIC
    // 00:00:06.015 Installed Extension Packs:
    // 00:00:06.015   None installed!
    //
    pid_start = output.find("Process ID: ");
    if (pid_start == string::npos) {
        return ERR_NOT_FOUND;
    }
    pid_start += 12;
    pid_end = output.find("\n", pid_start);
    pid = output.substr(pid_start, pid_end - pid_start);
    if (pid.size() <= 0) {
        return ERR_NOT_FOUND;
    }
    process_id = atol(pid.c_str());

#ifndef _WIN32
    vm_pid = process_id;
#else
    vm_pid_handle = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
        FALSE,
        process_id
    );
#endif

    return 0;
}

int VBOX_VM::get_port_forwarding_port() {
    sockaddr_in addr;
    BOINC_SOCKLEN_T addrsize;
    int sock;
    int retval;

    addrsize = sizeof(sockaddr_in);

    memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(pf_host_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    retval = boinc_socket(sock);
    if (retval) return retval;
 
    retval = bind(sock, (const sockaddr*)&addr, addrsize);
    if (retval < 0) {
        boinc_close_socket(sock);

        // Lets see if we can get anything useable at this point
        memset(&addr, 0, sizeof(sockaddr_in));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        retval = boinc_socket(sock);
        if (retval) return retval;
     
        retval = bind(sock, (const sockaddr*)&addr, addrsize);
        if (retval < 0) {
            boinc_close_socket(sock);
            return ERR_BIND;
        }
    }

    getsockname(sock, (sockaddr*)&addr, &addrsize);
    pf_host_port = ntohs(addr.sin_port);

    boinc_close_socket(sock);
    return 0;
}

int VBOX_VM::get_remote_desktop_port() {
    sockaddr_in addr;
    BOINC_SOCKLEN_T addrsize;
    int sock;
    int retval;

    addrsize = sizeof(sockaddr_in);

    memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    retval = boinc_socket(sock);
    if (retval) return retval;
 
    retval = bind(sock, (const sockaddr*)&addr, addrsize);
    if (retval < 0) {
        boinc_close_socket(sock);
        return ERR_BIND;
    }

    getsockname(sock, (sockaddr*)&addr, &addrsize);
    rd_host_port = ntohs(addr.sin_port);

    boinc_close_socket(sock);
    return 0;
}

// Enable the network adapter if a network connection is required.
// NOTE: Network access should never be allowed if the code running in a 
//   shared directory or the VM image itself is NOT signed.  Doing so
//   opens up the network behind the company firewall to attack.
//
//   Imagine a doomsday scenario where a project has been compromised and
//   an unsigned executable/VM image has been tampered with.  Volunteer
//   downloads compromised code and executes it on a company machine.
//   Now the compromised VM starts attacking other machines on the company
//   network.  The company firewall cannot help because the attacking
//   machine is already behind the company firewall.
//
int VBOX_VM::set_network_access(bool enabled) {
    string command;
    string output;
    char buf[256];
    int retval;

    network_suspended = !enabled;

    if (enabled) {
        fprintf(
            stderr,
            "%s Enabling network access for VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        command  = "modifyvm \"" + vm_name + "\" ";
        command += "--cableconnected1 on ";

        retval = vbm_popen(command, output, "enable network");
        if (retval) return retval;
    } else {
        fprintf(
            stderr,
            "%s Disabling network access for VM.\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf))
        );
        command  = "modifyvm \"" + vm_name + "\" ";
        command += "--cableconnected1 off ";

        retval = vbm_popen(command, output, "disable network");
        if (retval) return retval;
    }
    return 0;
}

int VBOX_VM::set_cpu_usage(int percentage) {
    string command;
    string output;
    char buf[256];
    int retval;

    // the arg to controlvm is percentage
    //
    fprintf(
        stderr,
        "%s Setting cpu throttle for VM. (%d%%)\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf)),
        percentage
    );
    sprintf(buf, "%d", percentage);
    command  = "controlvm \"" + vm_name + "\" ";
    command += "cpuexecutioncap ";
    command += buf;
    command += " ";

    retval = vbm_popen(command, output, "CPU throttle");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::set_network_usage(int kilobytes) {
    string command;
    string output;
    char buf[256];
    int retval;


    // the argument to modifyvm is in Kbps
    //
    fprintf(
        stderr,
        "%s Setting network throttle for VM.\n",
        vboxwrapper_msg_prefix(buf, sizeof(buf))
    );
    sprintf(buf, "%d", kilobytes);
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--nicspeed1 ";
    command += buf;
    command += " ";

    retval = vbm_popen(command, output, "network throttle");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::read_floppy(std::string& data) {
    if (enable_floppyio && pFloppy) {
        data = pFloppy->receive();
        return 0;
    }
    return 1;
}

int VBOX_VM::write_floppy(std::string& data) {
    if (enable_floppyio && pFloppy) {
        pFloppy->send(data);
        return 0;
    }
    return 1;
}

void VBOX_VM::lower_vm_process_priority() {
#ifndef _WIN32
    if (vm_pid) {
        setpriority(PRIO_PROCESS, vm_pid, PROCESS_IDLE_PRIORITY);
    }
#else
    if (vm_pid_handle) {
        SetPriorityClass(vm_pid_handle, IDLE_PRIORITY_CLASS);
    }
#endif
}

void VBOX_VM::reset_vm_process_priority() {
#ifndef _WIN32
    if (vm_pid) {
        setpriority(PRIO_PROCESS, vm_pid, PROCESS_MEDIUM_PRIORITY);
    }
#else
    if (vm_pid_handle) {
        SetPriorityClass(vm_pid_handle, NORMAL_PRIORITY_CLASS);
    }
#endif
}

// Launch VboxSVC.exe before going any further. if we don't, it'll be launched by
// svchost.exe with its environment block which will not contain the reference
// to VBOX_USER_HOME which is required for running in the BOINC account-based
// sandbox on Windows.
int VBOX_VM::launch_vboxsvc() {

#ifdef _WIN32
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    APP_INIT_DATA aid;
    char buf[256];
    string command;

    boinc_get_init_data_p(&aid);

    if (aid.using_sandbox) {

        if ((vboxsvc_handle == NULL) || !process_exists(vboxsvc_handle)) {

            if (vboxsvc_handle) CloseHandle(vboxsvc_handle);

            memset(&si, 0, sizeof(si));
            memset(&pi, 0, sizeof(pi));

            si.cb = sizeof(STARTUPINFO);
            si.dwFlags |= STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            command = "\"" + virtualbox_install_directory + "\\VBoxSVC.exe\" --logrotate 1 --logsize 1024000";

            if (!CreateProcess(NULL, (LPTSTR)command.c_str(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                fprintf(
                    stderr,
                    "%s Creating VBoxSVC.exe failed! (%d).\n",
                    vboxwrapper_msg_prefix(buf, sizeof(buf)),
                    GetLastError()
                );
            }

            vboxsvc_handle = pi.hProcess;

            if (pi.hThread) CloseHandle(pi.hThread);
        }
    }
#endif

    return 0;
}

// If there are errors we can recover from, process them here.
//
int VBOX_VM::vbm_popen(string& arguments, string& output, const char* item, bool log_error, bool retry_failures, unsigned int timeout) {
    int retval = 0;
    int retry_count = 0;
    double sleep_interval = 1.0;
    char buf[256];
    string retry_notes;

    do {
        retval = vbm_popen_raw(arguments, output, timeout);
        if (retval) {

            // VirtualBox designed the concept of sessions to prevent multiple applications using
            // the VirtualBox COM API (virtualbox.exe, vboxmanage.exe) from modifying the same VM
            // at the same time.
            //
            // The problem here is that vboxwrapper uses vboxmanage.exe to modify and control the
            // VM.  Vboxmanage.exe can only maintain the session lock for as long as it takes it
            // to run.  So that means 99% of the time that a VM is running under BOINC technology
            // it is running without a session lock.
            //
            // If a volunteer opens another VirtualBox management application and goes poking around
            // that application can aquire the session lock and not give it up for some time.
            //
            // If we detect that condition retry the desired command.
            //
            // Experiments performed by jujube suggest changing the sleep interval to an exponential
            // style backoff would increase our chances of success in situations where the previous
            // lock is held by a previous instance of vboxmanage whos instance data hasn't been
            // cleaned up within vboxsvc yet.
            //
            // Error Code: VBOX_E_INVALID_OBJECT_STATE (0x80bb0007) 
            //
            if (0x80bb0007 == retval) {
                if (retry_notes.find("Another VirtualBox management") == string::npos) {
                    retry_notes += "Another VirtualBox management application has locked the session for\n";
                    retry_notes += "this VM. BOINC cannot properly monitor this VM\n";
                    retry_notes += "and so this job will be aborted.\n\n";
                }
                if (retry_count) {
                    sleep_interval *= 2;
                }
            }
            
            // Retry?
            if (!retry_failures) break;

            // Timeout?
            if (retry_count >= 5) break;

            retry_count++;
            boinc_sleep(sleep_interval);
        }
    }
    while (retval);

#ifdef _WIN32
    // Remove \r from the log spew
    string::iterator iter = output.begin();
    while (iter != output.end()) {
        if (*iter == '\r') {
            iter = output.erase(iter);
        } else {
            ++iter;
        }
    }
#endif

    // Add all relivent notes to the output string and log errors
    //
    if (retval && log_error) {
        if (!retry_notes.empty()) {
            output += "\nNotes:\n\n" + retry_notes;
        }

        fprintf(
            stderr,
            "%s Error in %s for VM: %d\nArguments:\n%s\nOutput:\n%s\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf)),
            item,
            retval,
            arguments.c_str(),
            output.c_str()
        );
    }

    return retval;
}

// Execute the vbox manage application and copy the output to the buffer.
//
int VBOX_VM::vbm_popen_raw(string& arguments, string& output, unsigned int timeout) {
    char buf[256];
    string command;
    size_t errcode_start;
    size_t errcode_end;
    string errcode;
    int retval = 0;

    // Launch vboxsvc in case it was shutdown for being idle
    launch_vboxsvc();

    // Initialize command line
    command = "VBoxManage -q " + arguments;

    // Reset output buffer
    output.clear();

#ifdef _WIN32

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    void* pBuf = NULL;
    DWORD dwCount = 0;
    unsigned long ulExitCode = 0;
    unsigned long ulExitTimeout = 0;

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    memset(&sa, 0, sizeof(sa));
    memset(&sd, 0, sizeof(sd));

    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, true, NULL, false);

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = &sd;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, NULL)) {
        fprintf(
            stderr,
            "%s CreatePipe failed! (%d).\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf)),
            GetLastError()
        );
        goto CLEANUP;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(STARTUPINFO);
    si.dwFlags |= STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    // Execute command
    if (!CreateProcess(
        NULL, 
        (LPTSTR)command.c_str(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW, NULL,
        NULL,
        &si,
        &pi
    )) {
        fprintf(
            stderr,
            "%s CreateProcess failed! (%d).\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf)),
            GetLastError()
        );
        goto CLEANUP;
    }

    // Wait until process has completed
    while(1) {
        if (timeout == 0) {
            WaitForSingleObject(pi.hProcess, INFINITE);
        }

        GetExitCodeProcess(pi.hProcess, &ulExitCode);

        // Copy stdout/stderr to output buffer, handle in the loop so that we can
        // copy the pipe as it is populated and prevent the child process from blocking
        // in case the output is bigger than pipe buffer.
        PeekNamedPipe(hReadPipe, NULL, NULL, NULL, &dwCount, NULL);
        if (dwCount) {
            pBuf = malloc(dwCount+1);
            memset(pBuf, 0, dwCount+1);

            if (ReadFile(hReadPipe, pBuf, dwCount, &dwCount, NULL)) {
                output += (char*)pBuf;
            }

            free(pBuf);
        }

        if (ulExitCode != STILL_ACTIVE) break;

        // Timeout?
        if (ulExitTimeout >= (timeout * 1000)) {
            TerminateProcess(pi.hProcess, EXIT_FAILURE);
            ulExitCode = 0;
            retval = ERR_TIMEOUT;
            Sleep(1000);
        }

        Sleep(250);
        ulExitTimeout += 250;
    }

CLEANUP:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (hReadPipe) CloseHandle(hReadPipe);
    if (hWritePipe) CloseHandle(hWritePipe);

    if ((ulExitCode != 0) || (!pi.hProcess)) {

        // Determine the real error code by parsing the output
        errcode_start = output.find("(0x");
        if (errcode_start) {
            errcode_start += 1;
            errcode_end = output.find(")", errcode_start);
            errcode = output.substr(errcode_start, errcode_end - errcode_start);

            sscanf(errcode.c_str(), "%x", &retval);
        }

        // If something couldn't be found, just return ERR_FOPEN
        if (!retval) retval = ERR_FOPEN;

    }

#else

    FILE* fp;

    // redirect stderr to stdout for the child process so we can trap it with popen.
    string modified_command = command + " 2>&1";

    // Execute command
    fp = popen(modified_command.c_str(), "r");
    if (fp == NULL){
        fprintf(
            stderr,
            "%s vbm_popen popen failed! errno = %d\n",
            vboxwrapper_msg_prefix(buf, sizeof(buf)),
            errno
        );
        retval = ERR_FOPEN;
    } else {
        // Copy output to buffer
        while (fgets(buf, 256, fp)) {
            output += buf;
        }

        // Close stream
        pclose(fp);

        // Determine the real error code by parsing the output
        errcode_start = output.find("(0x");
        if (errcode_start) {
            errcode_start += 1;
            errcode_end = output.find(")", errcode_start);
            errcode = output.substr(errcode_start, errcode_end - errcode_start);

            sscanf(errcode.c_str(), "%x", &retval);
        }

        retval = 0;
    }

#endif

    return retval;
}
