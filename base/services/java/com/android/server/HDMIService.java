/*
 * Copyright 2007, The Android Open Source Project
 * Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
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

package com.android.server;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.net.Uri;
import android.os.Environment;
import android.os.IHDMIService;
import android.os.RemoteException;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.UEventObserver;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.Log;

import java.io.File;
import java.io.FileReader;
import java.util.StringTokenizer;

/**
 * @hide
 */
class HDMIService extends IHDMIService.Stub {

    private static final String TAG = "HDMIService";

    private Context mContext;

    private HDMIListener mListener;
    private boolean mHDMIUserOption = false;
    private int mHDMIModes[];
    final int HDMIIcon = 0x010800b1;
   // final int HDMIIcon = 17302346;
    public final String HDMICableConnectedEvent = "HDMI_CABLE_CONNECTED";
    public final String HDMICableDisconnectedEvent = "HDMI_CABLE_DISCONNECTED";
    public final String HDMIONEvent = "HDMI_CONNECTED";
    public final String HDMIOFFEvent = "HDMI_DISCONNECTED";

    final int m640x480p60_4_3         = 1;
    final int m720x480p60_4_3         = 2;
    final int m720x480p60_16_9        = 3;
    final int m1280x720p60_16_9       = 4;
    final int m1920x1080i60_16_9      = 5;
    final int m1440x480i60_4_3        = 6;
    final int m1440x480i60_16_9       = 7;
    final int m1920x1080p60_16_9      = 16;
    final int m720x576p50_4_3         = 17;
    final int m720x576p50_16_9        = 18;
    final int m1280x720p50_16_9       = 19;
    final int m1440x576i50_4_3        = 21;
    final int m1440x576i50_16_9       = 22;
    final int m1920x1080p50_16_9      = 31;
    final int m1920x1080p24_16_9      = 32;
    final int m1920x1080p25_16_9      = 33;
    final int m1920x1080p30_16_9      = 34;

    int getModeOrder(int mode)
    {
        switch (mode) {
        default:
        case m1440x480i60_4_3:
        case m1440x480i60_16_9:
            return 1; // 480i
        case m1440x576i50_4_3:
        case m1440x576i50_16_9:
            return 2; // 576i
        case m640x480p60_4_3:
            return 3; // 480p x640
        case m720x480p60_4_3:
        case m720x480p60_16_9:
            return 4; // 480p x720
        case m720x576p50_4_3:
        case m720x576p50_16_9:
            return 5; // 576p
        case m1920x1080i60_16_9:
            return 6; // 1080i
        case m1280x720p60_16_9:
        case m1280x720p50_16_9:
            return 7; // 720p
        case m1920x1080p24_16_9:
        case m1920x1080p25_16_9:
        case m1920x1080p30_16_9:
        case m1920x1080p50_16_9:
        case m1920x1080p60_16_9:
            return 8;
        }
    }

    int getBestMode()
    {
        int bestOrder = 0, bestMode = m640x480p60_4_3;
        for (int mode : mHDMIModes) {
            int order = getModeOrder(mode);
            if (order > bestOrder) {
                bestOrder = order;
                bestMode = mode;
            }
        }
        return bestMode;
    }

    public HDMIService(Context context) {
        mContext = context;
        // Register a BOOT_COMPLETED handler so that we can start
        // HDMIListener. We defer the startup so that we don't
        // start processing events before we ought-to
        
        //IntentFilter localIntentFilter = new IntentFilter();
        //localIntentFilter.addAction(Intent.ACTION_BOOT_COMPLETED);
        //localIntentFilter.addAction("HDMI_CHANGEMODE");
        
        mContext.registerReceiver(mBroadcastReceiver, new IntentFilter(Intent.ACTION_BOOT_COMPLETED), null, null);
        mListener =  new HDMIListener(this);
        
        String hdmiUserOption = Settings.System.getString(
                              mContext.getContentResolver(),
                  "HDMI_USEROPTION");
        if (hdmiUserOption != null && hdmiUserOption.equals("HDMI_ON"))
        	mHDMIUserOption = true;
    }

    BroadcastReceiver mBroadcastReceiver = new BroadcastReceiver() 
    {
        public void onReceive(Context context, Intent intent) 
        {
	      Log.e("BroadcastReceiverHDMI", "on Recieve:entered with action:  " + intent.getAction());
            String action = intent.getAction();

            String targetDevice = SystemProperties.get("ro.product.device");
            if (action.equals(Intent.ACTION_BOOT_COMPLETED)
                    && (targetDevice != null
                    && (targetDevice.startsWith("msm7630") || targetDevice.startsWith("msm8660") || targetDevice.equals("triumph")))) 
            {
            	Log.e("BroadcastReceiverHDMI", "on Recieve: about to spawn new thread");
                Thread thread = new Thread(mListener, HDMIListener.class.getName());
                thread.start();
            }
        }
    };

    public void shutdown() {
        if (mContext.checkCallingOrSelfPermission(
                android.Manifest.permission.SHUTDOWN)
                != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Requires SHUTDOWN permission");
        }

        Log.e(TAG, "Shutting down");
    }

    public boolean isHDMIConnected() 
    {
        if (mListener == null)
            return false;
        return mListener.isHDMIConnected();
    }

    public void setHDMIOutput(boolean enableHDMI) 
    {
        Settings.System.putString(mContext.getContentResolver(),
            "HDMI_USEROPTION", enableHDMI ? "HDMI_ON" : "HDMI_OFF");
        mHDMIUserOption = enableHDMI;

        if(this.mListener != null)
        {
	        synchronized(mListener) 
	        {
	            if(enableHDMI == false || isHDMIConnected() == false) 
	            {
	            	broadcastEvent(HDMICableDisconnectedEvent);
	                broadcastEvent(HDMIOFFEvent);              
	                mListener.enableHDMIOutput(false);
	            }
	            else
	            {
	            	broadcastEvent(HDMIONEvent, this.mHDMIModes);
	            	broadcastEvent(HDMICableConnectedEvent);
	            	mListener.enableHDMIOutput(true);
	            }
	            //mListener.setHPD(getHDMIUserOption());
	        }
        }
    }

    public void notifyWakeupsystem()
    {
      broadcastEvent("HDMI_ONLINE");
    }

    public void setActionsafeWidthRatio(float asWidthRatio){
        mListener.setActionsafeWidthRatio(asWidthRatio);
    }

    public void setActionsafeHeightRatio(float asHeightRatio){
        mListener.setActionsafeHeightRatio(asHeightRatio);
    }

    public boolean getHDMIUserOption() {
        return mHDMIUserOption;
    }
    
    public void RefreshFramebuffer()
    {
    	Log.e(TAG, "Refresh Frame Buffer Called");
    	
      NotificationManager localNotificationManager = (NotificationManager)this.mContext.getSystemService("notification");
      localNotificationManager.cancel(HDMIIcon);
      SystemClock.sleep(1000L);
      Log.e("HDMIService", "RefreshFramebuffer ... ");
      Intent localIntent = new Intent();
      localIntent.setClassName("com.android.settings", "com.android.settings.HDMIOutputSetting");
      localIntent.setFlags(270532608);
      long l = System.currentTimeMillis();
      PendingIntent localPendingIntent = PendingIntent.getActivity(this.mContext, HDMIIcon, localIntent, 0);
      Notification localNotification = new Notification(HDMIIcon, "HDMI CONNECTED", l);
      localNotification.flags = 34;
      localNotification.setLatestEventInfo(this.mContext, "HDMI INFO", "HDMI Cable is now connected", localPendingIntent);//this.mContext.getResources().getString(17042317), this.mContext.getResources().getString(17042318), localPendingIntent);
      localNotificationManager.notify(HDMIIcon, localNotification);
    }

    public void broadcastEvent(String eventName) {
        Intent intent = new Intent(eventName);
        intent.addCategory(Intent.CATEGORY_DEFAULT);
        mContext.sendBroadcast(intent);
        Log.e(TAG, "Broadcasting ... " + eventName);
    }

    public void broadcastEvent(String eventName, int[] modes) {
        Intent intent = new Intent(eventName);
        intent.addCategory(Intent.CATEGORY_DEFAULT);
        intent.putExtra("EDID", modes);
        mContext.sendBroadcast(intent);
        Log.e(TAG, "Broadcasting ... " + eventName + ", modes: " + modes.length);
    }

    public void notifyHDMIConnected(int[] modes) 
    {
        mHDMIModes = modes;
        
        
        NotificationManager localNotificationManager = (NotificationManager)this.mContext.getSystemService("notification");
        Intent localIntent = new Intent();
        localIntent.setClassName("com.android.settings", "com.android.settings.HDMIOutputSetting");
        localIntent.setFlags(270532608);

        long l = System.currentTimeMillis();
        PendingIntent localPendingIntent = PendingIntent.getActivity(this.mContext, HDMIIcon, localIntent, 0);
        Notification localNotification = new Notification(HDMIIcon, "HDMI CONNECTED", l);//this.mContext.getResources().getString(17042317), l);
        localNotification.flags = 34;
        localNotification.setLatestEventInfo(this.mContext, "HDMI INFO", "HDMI Cable is now connected", localPendingIntent); //this.mContext.getResources().getString(17042317), this.mContext.getResources().getString(17042318), localPendingIntent);
        localNotificationManager.notify(HDMIIcon, localNotification);
        Log.d("HDMIService", "notifyHDMIConnected ... Broadcasting On");

        setHDMIOutput(true);   
    }

    public void notifyHDMIDisconnected() 
    {
        mHDMIModes = null;
        
        ((NotificationManager)this.mContext.getSystemService("notification")).cancel(HDMIIcon);
        setHDMIOutput(false);
        
    }

    public void notifyHDMIAudioOn() {
        if(getHDMIUserOption()) {
            broadcastEvent(HDMIONEvent, mHDMIModes);
        }
    }

    public void notifyHDMIAudioOff() {
        if(getHDMIUserOption()) {
            broadcastEvent(HDMIOFFEvent);
       }
    }
}
