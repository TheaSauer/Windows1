﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"
#include <winrt/Windows.Foundation.Metadata.h>
#include "PushNotificationChannel.h"
#include "Microsoft.Windows.PushNotifications.PushNotificationChannel.g.cpp"
#include <winrt\Windows.Foundation.h>
#include <FrameworkUdk/PushNotifications.h>
#include "externs.h"
#include "PushNotificationTelemetry.h"
#include "PushNotificationUtility.h"

using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::Windows::PushNotifications;


namespace PushNotificationHelpers
{
    using namespace winrt::Microsoft::Windows::PushNotifications::Helpers;
}

namespace winrt::Microsoft::Windows::PushNotifications::implementation
{
    PushNotificationChannel::PushNotificationChannel(struct ChannelDetails channelInfo)
    {
        std::swap(m_channelInfo, channelInfo);
    }

    Uri PushNotificationChannel::Uri()
    {
        return winrt::Windows::Foundation::Uri{ m_channelInfo.channelUri };
    }

    DateTime PushNotificationChannel::ExpirationTime()
    {
        return m_channelInfo.channelExpiryTime;
    }

    void PushNotificationChannel::Close()
    {
        HRESULT hr{ S_OK };

        auto logTelemetry{ wil::scope_exit([&]() {
            PushNotificationTelemetry::LogCloseChannel(hr);
        }) };

        try
        {
            THROW_IF_FAILED(PushNotifications_CloseChannel(m_channelInfo.appId.c_str(), m_channelInfo.channelId.c_str()));
        }

        catch (...)
        {
            auto channelCloseException { hresult_error(to_hresult()) };

            hr = channelCloseException.code();

            if (channelCloseException.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                throw channelCloseException;
            }
        }
    }
}
