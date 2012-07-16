/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "HDMIDaemon"

#include <stdint.h>
#include <sys/types.h>
#include <math.h>
#include <fcntl.h>
#include <utils/misc.h>
#include <signal.h>

#include <binder/IPCThreadState.h>
#include <utils/threads.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/AssetManager.h>

#include <ui/DisplayInfo.h>
#include <ui/FramebufferNativeWindow.h>
#include <linux/msm_mdp.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include <cutils/properties.h>

#include "HDMIDaemon.h"

namespace android {

// ---------------------------------------------------------------------------


HDMIDaemon::HDMIDaemon() : Thread(false), mHDMISocketName("hdmid"),
           mFrameworkSock(-1), mAcceptedConnection(-1), mUeventSock(-1),
	   mHDMIUeventQueueHead(NULL), mHDMIConnected("hdmi_connected"),
	   mHDMIDisConnected("hdmi_disconnected"),
	   mHDMIStateFile("/sys/kernel/hdmi_kset/hdmi_kobj/hdmi_state_obj")
{
    fd0 = open("/dev/graphics/fb0", O_RDWR);
    fd1 = open("/dev/graphics/fb1", O_RDWR);
}

HDMIDaemon::~HDMIDaemon() {
    HDMIUeventQueue* tmp = mHDMIUeventQueueHead, *tmp1;
    while (tmp != NULL) {
	tmp1 = tmp;
	tmp = tmp->next;
        delete tmp1;
    }
    mHDMIUeventQueueHead = NULL;
    close(fd0);
    close(fd1);
}

void HDMIDaemon::onFirstRef() {
    run("HDMIDaemon", PRIORITY_AUDIO);
}

sp<SurfaceComposerClient> HDMIDaemon::session() const {
    return mSession;
}


void HDMIDaemon::binderDied(const wp<IBinder>& who)
{
    requestExit();
}

status_t HDMIDaemon::readyToRun() {

    if (fd0 < 0 || fd1 < 0 ||
          (mFrameworkSock = android_get_control_socket(mHDMISocketName)) < 0) {
        LOGE("Obtaining file descriptor socket '%s' failed: %s",
             mHDMISocketName, strerror(errno));
        return -1;
    }

    if (listen(mFrameworkSock, 4) < 0) {
        LOGE("Unable to listen on fd '%d' for socket '%s': %s",
             mFrameworkSock, mHDMISocketName, strerror(errno));
        return -1;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    nladdr.nl_pid = getpid();
    nladdr.nl_groups = 0xffffffff;

    if ((mUeventSock = socket(PF_NETLINK,
                             SOCK_DGRAM,NETLINK_KOBJECT_UEVENT)) < 0) {
        LOGE("Unable to create uevent socket: %s", strerror(errno));
        return -1;
    }

    int uevent_sz = 64 * 1024;
    if (setsockopt(mUeventSock, SOL_SOCKET, SO_RCVBUFFORCE, &uevent_sz,
                   sizeof(uevent_sz)) < 0) {
        LOGE("Unable to set uevent socket options: %s", strerror(errno));
        return -1;
    }

    if (bind(mUeventSock, (struct sockaddr *) &nladdr, sizeof(nladdr)) < 0) {
        LOGE("Unable to bind uevent socket: %s", strerror(errno));
        return -1;
    }

    return NO_ERROR;
}

bool HDMIDaemon::threadLoop()
{
    int max = -1;
    fd_set read_fds;
    FD_ZERO(&read_fds);

    FD_SET(mFrameworkSock, &read_fds);
    if (max < mFrameworkSock)
        max = mFrameworkSock;
    FD_SET(mUeventSock, &read_fds);
    if (max < mUeventSock)
        max = mUeventSock;

    if (mAcceptedConnection != -1) {
        FD_SET(mAcceptedConnection, &read_fds);
	if (max < mAcceptedConnection)
	    max = mAcceptedConnection;
    }

    struct timeval to;
    to.tv_sec = (60 * 60);
    to.tv_usec = 0;

    int ret;
    if ((ret = select(max + 1, &read_fds, NULL, NULL, &to)) < 0) {
        LOGE("select() failed (%s)", strerror(errno));
        sleep(1);
        return true;
    }

    if (!ret) {
        return true;
    }

    if (mAcceptedConnection != -1 && FD_ISSET(mAcceptedConnection, &read_fds)) {
        if (processFrameworkCommand() == -1)
	    mAcceptedConnection = -1;
    }

    if (FD_ISSET(mFrameworkSock, &read_fds)) {
        struct sockaddr addr;
        socklen_t alen;
        alen = sizeof(addr);

        if (mAcceptedConnection != -1) {
	    close(mAcceptedConnection);
            mAcceptedConnection = accept(mFrameworkSock, &addr, &alen);
	    return true;
        }

        if ((mAcceptedConnection = accept(mFrameworkSock, &addr, &alen)) < 0) {
            LOGE("Unable to accept framework connection (%s)",
            strerror(errno));
        }
	else {
	    mSession = new SurfaceComposerClient();
	    processUeventQueue();

	    // Until HPD gets stabilized we are going
	    // to send the event as always connected

	    sendCommandToFramework(true);

	    int hdmiStateFile = open(mHDMIStateFile, O_RDONLY, 0);
	    if (hdmiStateFile < 0)
	        LOGE("hdmi state file not present... ");
	    else {
		char buf;
	        int err = read(hdmiStateFile, &buf, 1);
		if (err <= 0)
		    LOGE("No read from hdmi state");
	        else {
		    if (buf == '1')
		        //sendCommandToFramework(true);
		        LOGE("evnentLOG from Driver:: HDMI Cable connected..");
		    else
		        //sendCommandToFramework(false);
		        LOGE("evnentLOG from Driver::HDMI Cable disconnected..");
		}
	    }
        }

        LOGE("Accepted connection from framework");
    }

    if (FD_ISSET(mUeventSock, &read_fds)) {
        if (mAcceptedConnection == -1)
            queueUevent();
        else
            processUevent();
    }

    return true;
}

bool HDMIDaemon::processUeventMessage(uevent& event)
{
    char buffer[64 * 1024];
    int count;
    char *s = buffer;
    char *end;
    int param_idx = 0;
    int i;
    bool first = true;

    if ((count = recv(mUeventSock, buffer, sizeof(buffer), 0)) < 0) {
        LOGE("Error receiving uevent (%s)", strerror(errno));
        return false;
    }

    end = s + count;
    while (s < end) {
        if (first) {
            char *p;
            for (p = s; *p != '@'; p++);
            p++;
	    if (!strcasestr(p, "hdmi_kobj")) {
	        return false;
	    }
	    event.path = new char[strlen(p) + 1];
	    strcpy(event.path, p);
            first = false;
        } else {
            if (!strncmp(s, "ACTION=", strlen("ACTION="))) {
                char *a = s + strlen("ACTION=");

                if (!strcmp(a, "add"))
                    event.action = action_add;
                else if (!strcmp(a, "change"))
                    event.action = action_change;
                else if (!strcmp(a, "remove"))
                    event.action = action_remove;
                else if (!strcmp(a, "online"))
                    event.action = action_online;
                else if (!strcmp(a, "offline"))
                    event.action = action_offline;
            } else if (!strncmp(s, "SEQNUM=", strlen("SEQNUM=")))
                event.seqnum = atoi(s + strlen("SEQNUM="));
            else if (!strncmp(s, "SUBSYSTEM=", strlen("SUBSYSTEM="))) {
		event.subsystem = new char[strlen(s + strlen("SUBSYSTEM=")) + 1];
		strcpy(event.subsystem, (s + strlen("SUBSYSTEM=")));
	    }
            else {
		event.param[param_idx] = new char[strlen(s) + 1];
		strcpy(event.param[param_idx], s);
		param_idx++;
	    }
        }
        s+= strlen(s) + 1;
    }
    return true;
}

void HDMIDaemon::queueUevent()
{
    HDMIUeventQueue* tmp = mHDMIUeventQueueHead, *tmp1;
    while (tmp != NULL && tmp->next != NULL)
        tmp = tmp->next;
    if (!tmp) {
        tmp = new HDMIUeventQueue();
	tmp->next = NULL;
	if(!processUeventMessage(tmp->mEvent))
	    delete tmp;
	else
            mHDMIUeventQueueHead = tmp;
    }
    else {
        tmp1 = new HDMIUeventQueue();
	tmp1->next = NULL;
	if(!processUeventMessage(tmp1->mEvent))
	    delete tmp1;
        else
	    tmp->next = tmp1;
    }
}

void HDMIDaemon::processUeventQueue()
{
    HDMIUeventQueue* tmp = mHDMIUeventQueueHead, *tmp1;
    while (tmp != NULL) {
	tmp1 = tmp;
	// Code commented until HPD stabilizes
        /*if (tmp->mEvent.action == action_offline)
	    sendCommandToFramework(false);
        else if (tmp->mEvent.action == action_online)
	    sendCommandToFramework(true);*/
	tmp = tmp->next;
        delete tmp1;
    }
    mHDMIUeventQueueHead = NULL;
}

void HDMIDaemon::processUevent()
{
    uevent event;
    if(processUeventMessage(event)) {
        if (event.action == action_offline)
	    LOGE("evnentLOG from Driver::HDMI Cable disconnected");
	    //sendCommandToFramework(false);
        else if (event.action == action_online) {
	    //sendCommandToFramework(true);
	    LOGE("evnentLOG from Driver::HDMI Cable connected");
        }
    }
}

int HDMIDaemon::processFrameworkCommand()
{
    char buffer[128];
    int ret;

    if ((ret = read(mAcceptedConnection, buffer, sizeof(buffer) -1)) < 0) {
        LOGE("Unable to read framework command (%s)", strerror(errno));
        return -1;
    }
    else if (!ret)
        return -1;

    buffer[ret] = 0;
    int en = 0;
    if (!strcmp(buffer, "enable_hdmi")) {
	int hdmiStateFile = open(mHDMIStateFile, O_RDONLY, 0);
	int xres = 1280, yres = 720;
	if (hdmiStateFile < 0)
            LOGE("Couldn't open the HDMIState file");
        else {
	    char buf[1024];
            xres = 0; yres = 0;
	    memset(buf, 0, 1024);
	    int err = read(hdmiStateFile, buf, 1024);
	    if (err <= 0)
	        LOGE("No state information from hdmi state file");
	    else {
	        char* c = strstr(buf, "RES=");
		if (c) {
		    c += 4;
		    while ((*c) && ((*c) != 'x')) {
		        xres = (xres * 10) + (*c) - '0';
			c++;
		    }
		    c++;
		    while ((*c)) {
		        yres = (yres * 10) + (*c) - '0';
			c++;
		    }
		}
		else
		    LOGE("State information not accurate, \
		                      no resolution information");
	    }
        }
	close(hdmiStateFile);

        struct fb_var_screeninfo info;
        ioctl(fd1, FBIOBLANK, FB_BLANK_UNBLANK);
        ioctl(fd1, FBIOGET_VSCREENINFO, &info);
        info.reserved[0] = 0;
        info.reserved[1] = 0;
        info.reserved[2] = 0;
        info.xoffset = 0;
        info.yoffset = 0;
        info.xres = xres;
        info.yres = yres;
        info.activate = FB_ACTIVATE_NOW;
        ioctl(fd1, FBIOPUT_VSCREENINFO, &info);
	en = 1;
	ioctl(fd1, MSMFB_OVERLAY_PLAY_ENABLE, &en);
	property_set("hw.hdmiON", "1");
    } else if (!strcmp(buffer, "disable_hdmi")) {
        ioctl(fd1, MSMFB_OVERLAY_PLAY_ENABLE, &en);
        property_set("hw.hdmiON", "0");
        ioctl(fd1, FBIOBLANK, FB_BLANK_POWERDOWN);
    }

    return 0;
}

bool HDMIDaemon::sendCommandToFramework(bool connected)
{
    const char* message;
    if (!connected)
        message = mHDMIDisConnected;
    else
        message = mHDMIConnected;

    int result = write(mAcceptedConnection, message, strlen(message) + 1);
    if (result < 0)
        return false;
    return true;
}

// ---------------------------------------------------------------------------

}
; // namespace android
