/*
 * Copyright (C) 2024-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package com.nothing.kafka;

import android.content.Context;
import android.os.Handler;

/** Compatibility stub for the Nothing framework event observer. */
public class EventObserver {
    public EventObserver(Context context, Handler handler, String tag) {
    }

    public void register(EventCallback callback, boolean enabled) {
    }

    public void unregister() {
    }

    public interface EventCallback {
        void updateEvent(String event);
    }
}
