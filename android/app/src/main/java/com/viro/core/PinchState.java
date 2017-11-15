/*
 * Copyright (c) 2017-present, ViroMedia, Inc.
 * All rights reserved.
 */

package com.viro.core;

import java.util.HashMap;
import java.util.Map;

/**
 * Indicates that status of a pinch gesture made on a screen, for use with the {@link GesturePinchListener}.
 */
public enum PinchState {
    /**
     * Two fingers have gone down and have pinched inward or outward, beginning the pinch.
     */
    PINCH_START(1),
    /**
     * The pinch is continuing to move.
     */
    PINCH_MOVE(2),
    /**
     * The fingers have gone up, ending the pinch.
     */
    PINCH_END(3);

    private final int mTypeId;
    PinchState(int id) {
        mTypeId = id;
    }

    private static Map<Integer, PinchState> map = new HashMap<Integer, PinchState>();
    static {
        for (PinchState status : PinchState.values()) {
            map.put(status.mTypeId, status);
        }
    }

    /**
     * @hide
     */
    public static PinchState valueOf(int id) {
        return map.get(id);
    }
    public int getTypeId() { return mTypeId; }
}
