/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <haze.hpp>
#include <haze/console_main_loop.hpp>

#define INNER_HEAP_SIZE 0x80000

// Sysmodules should not use applet*.
u32 __nx_applet_type = AppletType_None;

// Sysmodules will normally only want to use one FS session.
u32 __nx_fs_num_sessions = 1;

extern "C" {

    void __libnx_initheap(void)
    {
        static u8 inner_heap[INNER_HEAP_SIZE];
        extern void* fake_heap_start;
        extern void* fake_heap_end;

        // Configure the newlib heap.
        fake_heap_start = inner_heap;
        fake_heap_end   = inner_heap + sizeof(inner_heap);
    }

    // Service initialization.
    void __appInit(void)
    {
        Result rc;

        // Open a service manager session.
        rc = smInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

        // Retrieve the current version of Horizon OS.
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }

        rc = pdmqryInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(rc);

        // Enable this if you want to use HID.
        /*rc = hidInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_HID));*/

        // Enable this if you want to use time.
        /*rc = timeInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_Time));

        __libnx_init_time();*/

        // Disable this if you don't want to use the filesystem.
        rc = fsInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

        // Disable this if you don't want to use the SD card filesystem.
        fsdevMountSdmc();
    }

    // Service deinitialization.
    void __appExit(void)
    {
        smExit();
        // Close extra services you added to __appInit here.
        fsdevUnmountAll(); // Disable this if you don't want to use the SD card filesystem.
        fsExit(); // Disable this if you don't want to use the filesystem.
    }
}

int main(int argc, char **argv) {
    /* Load device firmware version and serial number. */
    HAZE_R_ABORT_UNLESS(haze::LoadDeviceProperties());

    /* Run the application. */
    haze::ConsoleMainLoop::RunApplication();

    
    /* Return to the loader. */
    return 0;
}
