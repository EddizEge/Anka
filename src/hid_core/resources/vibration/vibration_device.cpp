// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_result.h"
#include "hid_core/resources/npad/npad_types.h"
#include "hid_core/resources/npad/npad_vibration.h"
#include "hid_core/resources/vibration/vibration_device.h"

namespace Service::HID {
namespace {
// A guest value (HID 214's layout is only known from captures) can be anything: NaN never equals
// itself, so the controller's duplicate filter never matched it, and the controller driver got
// it as is. Amplitudes are kept in [0, 1]; a frequency that is not a finite positive number
// becomes the default one.
Core::HID::VibrationValue SanitizeVibration(Core::HID::VibrationValue value) {
    const auto amplitude = [](f32 a) { return std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 0.0f; };
    const auto frequency = [](f32 f, f32 fallback) {
        return std::isfinite(f) && f > 0.0f ? f : fallback;
    };
    value.low_amplitude = amplitude(value.low_amplitude);
    value.high_amplitude = amplitude(value.high_amplitude);
    value.low_frequency =
        frequency(value.low_frequency, Core::HID::DEFAULT_VIBRATION_VALUE.low_frequency);
    value.high_frequency =
        frequency(value.high_frequency, Core::HID::DEFAULT_VIBRATION_VALUE.high_frequency);
    return value;
}
} // Anonymous namespace

NpadVibrationDevice::NpadVibrationDevice() {}

Result NpadVibrationDevice::Activate() {
    if (ref_counter == 0 && is_mounted) {
        f32 volume = 1.0f;
        const auto result = vibration_handler->GetVibrationVolume(volume);
        if (result.IsSuccess()) {
            xcd_handle->SetVibration(device_index, Core::HID::DEFAULT_VIBRATION_VALUE);
            // TODO: SendNotificationPattern;
        }
    }

    ref_counter++;
    return ResultSuccess;
}

Result NpadVibrationDevice::Deactivate() {
    if (ref_counter == 1 && is_mounted) {
        f32 volume = 1.0f;
        const auto result = vibration_handler->GetVibrationVolume(volume);
        if (result.IsSuccess()) {
            xcd_handle->SetVibration(device_index, Core::HID::DEFAULT_VIBRATION_VALUE);
            // TODO: SendNotificationPattern;
        }
    }

    if (ref_counter > 0) {
        ref_counter--;
    }

    return ResultSuccess;
}

Result NpadVibrationDevice::Mount(IAbstractedPad& abstracted_pad, Core::HID::DeviceIndex index,
                                  NpadVibration* handler) {
    if (!abstracted_pad.internal_flags.is_connected) {
        return ResultSuccess;
    }
    xcd_handle = abstracted_pad.xcd_handle;
    device_index = index;
    vibration_handler = handler;
    is_mounted = true;

    if (ref_counter == 0) {
        return ResultSuccess;
    }

    f32 volume{1.0f};
    const auto result = vibration_handler->GetVibrationVolume(volume);
    if (result.IsSuccess()) {
        xcd_handle->SetVibration(false);
    }

    return ResultSuccess;
}

Result NpadVibrationDevice::Unmount() {
    if (ref_counter == 0 || !is_mounted) {
        is_mounted = false;
        return ResultSuccess;
    }

    f32 volume{1.0f};
    const auto result = vibration_handler->GetVibrationVolume(volume);
    if (result.IsSuccess()) {
        xcd_handle->SetVibration(device_index, Core::HID::DEFAULT_VIBRATION_VALUE);
    }

    is_mounted = false;
    return ResultSuccess;
}

Result NpadVibrationDevice::SendVibrationValue(const Core::HID::VibrationValue& value) {
    if (ref_counter == 0) {
        return ResultVibrationNotInitialized;
    }
    if (!is_mounted) {
        return ResultSuccess;
    }

    f32 volume = 1.0f;
    const auto result = vibration_handler->GetVibrationVolume(volume);
    if (result.IsError()) {
        return result;
    }
    if (volume <= 0.0f) {
        xcd_handle->SetVibration(device_index, Core::HID::DEFAULT_VIBRATION_VALUE);
        return ResultSuccess;
    }

    Core::HID::VibrationValue vibration_value = SanitizeVibration(value);
    vibration_value.high_amplitude *= volume;
    vibration_value.low_amplitude *= volume;

    xcd_handle->SetVibration(device_index, vibration_value);
    return ResultSuccess;
}

Result NpadVibrationDevice::SendVibrationNotificationPattern([[maybe_unused]] u32 pattern) {
    if (!is_mounted) {
        return ResultSuccess;
    }

    f32 volume = 1.0f;
    const auto result = vibration_handler->GetVibrationVolume(volume);
    if (result.IsError()) {
        return result;
    }
    if (volume <= 0.0) {
        pattern = 0;
    }

    // TODO: SendVibrationNotificationPattern;
    return ResultSuccess;
}

Result NpadVibrationDevice::GetActualVibrationValue(Core::HID::VibrationValue& out_value) const {
    if (ref_counter < 1) {
        return ResultVibrationNotInitialized;
    }

    out_value = Core::HID::DEFAULT_VIBRATION_VALUE;
    if (!is_mounted) {
        return ResultSuccess;
    }

    out_value = xcd_handle->GetActualVibrationValue(device_index);
    return ResultSuccess;
}

} // namespace Service::HID
