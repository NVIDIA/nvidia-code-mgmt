/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "config.h"

#include "../nvidia_igx/igx/src/igx_util.hpp"
#include "../nvidia_igx/igx/src/platform_client.hpp"
#include "base_item_updater.hpp"

#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/bus/match.hpp>

#include <filesystem>
#include <memory>
#include <tuple>
#include <vector>

namespace nvidia
{
namespace software
{
namespace updater
{

/**
 * @brief SMCU updater
 * @author
 * @since Jan 12 2024
 */
class SMCUItemUpdater : public BaseItemUpdater
{
    /** @brief Tracks the model detected from the current image being
     *         processed
     */
    std::string currentModelToken;

    /** @brief Tracks the SMCU type detected from the platform at startup */
    std::string detectedPlatformSmcu;

    /** @brief Self-heal subscriptions: re-resolve + register the correct single
     *         SMCU object once the platform-state daemon publishes a real
     *         identity (Degraded->Ready) or restarts. Never registers both
     *         (bug 6397179 / ARCHITECTURE Sec 10a.1). */
    std::unique_ptr<sdbusplus::bus::match_t> platformPropsMatch;
    std::unique_ptr<sdbusplus::bus::match_t> smcuPropsMatch;
    std::unique_ptr<sdbusplus::bus::match_t> platformOwnerMatch;
    /** @brief Retries deferred onPlatformChanged once Activation→Active. */
    std::unique_ptr<sdbusplus::bus::match_t> activationMatch;
    /** @brief Retries deferred convergence once a Failed/Ready obj is gone. */
    std::unique_ptr<sdbusplus::bus::match_t> softwareRemovedMatch;
    bool pendingPlatformConvergence = false;

  private:
    /**
     * @brief Detect SMCU type from platform detection
     * @return "ASMCU" (Aurix) or "RSMCU" (Renesas)
     */
    std::string detectPlatformSmcu()
    {
        try
        {
            // Read the SMCU model from the platform-state daemon (D-Bus)
            // instead of spawning igx-platform-detection.py.
            std::string smcuType = nvidia::platform::smcuModel(bus);

            if (smcuType == "RSMCU")
            {
                lg2::info(
                    "Detected SMCU type: Renesas (from D-Bus Smcu.Model)");
                return "RSMCU";
            }
            else if (smcuType == "ASMCU")
            {
                lg2::info("Detected SMCU type: Aurix (from D-Bus Smcu.Model)");
                return "ASMCU";
            }

            // Priority 2: Fallback to platform name detection
            lg2::info(
                "SMCU_SOC field unavailable, falling back to platform name detection");

            std::string platformName = nvidia::platform::platformShort(bus);

            if (platformName == "Thor")
            {
                lg2::info(
                    "Detected Thor platform, using Renesas SMCU (fallback)");
                return "RSMCU";
            }
            else if (platformName == "Orin")
            {
                lg2::info(
                    "Detected Orin platform, using Aurix SMCU (fallback)");
                return "ASMCU";
            }
            else
            {
                lg2::error(
                    "Unknown platform: {PLATFORM}. Cannot determine SMCU type",
                    "PLATFORM", platformName.empty() ? "empty" : platformName);
                return "";
            }
        }
        catch (const std::exception& e)
        {
            lg2::error("Failed to detect platform SMCU: {ERROR}", "ERROR",
                       e.what());
            return "";
        }
    }

    bool hasNonActiveVersion() const
    {
        // Only Ready/Activating own the lifecycle. Failed is terminal — do not
        // block convergence (deleting a Failed object must recreate inventory).
        for (const auto& entry : versions)
        {
            const auto st = entry.second->activation();
            if (st == Version::Status::Ready ||
                st == Version::Status::Activating)
            {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Converge identity and refresh the live firmware version.
     *
     * Creates at most one deferred object; later signals update it in place.
     */
    void onPlatformChanged()
    {
        try
        {
            std::string t = detectPlatformSmcu();
            if (t != "ASMCU" && t != "RSMCU")
            {
                // Retain existing Version object across transient unready
                // (daemon restart). Replace only on positively ready change.
                return;
            }
            if (t != detectedPlatformSmcu)
            {
                // Do not replace inventory while an upload owns lifecycle
                // state (Ready or Activating). Failed is terminal.
                if (hasNonActiveVersion())
                {
                    lg2::warning(
                        "Deferring SMCU model change while update object is active");
                    pendingPlatformConvergence = true;
                    return;
                }
                if (!detectedPlatformSmcu.empty())
                {
                    erase(detectedPlatformSmcu);
                }
                detectedPlatformSmcu = t;
                std::string path = std::string(SOFTWARE_OBJPATH) + "/" + t;
                lg2::info("SMCU identity now known ({M}); registering deferred "
                          "inventory object",
                          "M", t);
                readDeviceDetails(path);
            }
            else if (versions.find(t) == versions.end())
            {
                // Same type cached but object missing (Ready at construction
                // with empty cache, or prior erase without recreate).
                std::string path = std::string(SOFTWARE_OBJPATH) + "/" + t;
                readDeviceDetails(path);
            }

            // processImage reuses the ASMCU/RSMCU id, so during flash the
            // object is Activating with the image filename as Version.
            // RedetectSmcu often lands in that window; refresh only when
            // Active, otherwise retry on Activation→Active (activationMatch).
            auto version = versions.find(detectedPlatformSmcu);
            if (version == versions.end())
            {
                return;
            }
            if (version->second->activation() != Version::Status::Active)
            {
                pendingPlatformConvergence = true;
                lg2::info(
                    "Deferring SMCU version refresh until inventory is Active");
                return;
            }
            const std::string path =
                std::string(SOFTWARE_OBJPATH) + "/" + detectedPlatformSmcu;
            version->second->updateVersionString(getVersion(path));
            pendingPlatformConvergence = false;
        }
        catch (const std::exception& e)
        {
            lg2::error("SMCU platform-change handler failed: {ERROR}", "ERROR",
                       e.what());
        }
    }

  public:
    /**
     * @brief Construct a new SMCUItemUpdater object
     *
     * @param bus
     */
    SMCUItemUpdater(sdbusplus::bus_t& bus) :
        BaseItemUpdater(bus, SMCU_SUPPORTED_MODEL, SMCU_INVENTORY_IFACE, "SMCU",
                        SMCU_BUSNAME_UPDATER, SMCU_UPDATE_SERVICE, false,
                        SMCU_BUSNAME_INVENTORY)
    {
        // Leave detectedPlatformSmcu empty; onPlatformChanged registers when
        // identity is known. Pre-seeding then calling onPlatformChanged skips
        // registration when the daemon is already Ready (t == cache).

        // Self-heal: if identity is unknown at construction we register nothing
        // (below); converge once the daemon publishes a real identity or
        // restarts. D-Bus signals do not replay, so also watch
        // NameOwnerChanged.
        namespace rules = sdbusplus::bus::match::rules;
        platformPropsMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            rules::propertiesChanged(nvidia::platform::objPath,
                                     nvidia::platform::infoIface),
            [this](sdbusplus::message::message&) { onPlatformChanged(); });
        smcuPropsMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            rules::propertiesChanged(nvidia::platform::objPath,
                                     nvidia::platform::smcuIface),
            [this](sdbusplus::message::message&) { onPlatformChanged(); });
        platformOwnerMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            rules::nameOwnerChanged() +
                rules::argN(0, nvidia::platform::service),
            [this](sdbusplus::message::message& msg) {
                std::string name;
                std::string oldOwner;
                std::string newOwner;
                try
                {
                    msg.read(name, oldOwner, newOwner);
                }
                catch (const std::exception&)
                {
                    return;
                }
                if (name == nvidia::platform::service && !newOwner.empty())
                {
                    onPlatformChanged();
                }
            });
        // Retry deferred convergence when flash Activation becomes Active
        // (no further platform PropertiesChanged after finishActivation).
        activationMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',path_namespace='" +
                std::string(SOFTWARE_OBJPATH) + "',arg0namespace='" +
                std::string(ACTIVATE_INTERFACE) + "'",
            [this](sdbusplus::message::message&) {
                if (pendingPlatformConvergence)
                {
                    onPlatformChanged();
                }
            });
        // Deleting the current Ready/Failed software object emits
        // InterfacesRemoved (not PropertiesChanged); always converge so the
        // sole inventory entry is recreated even when pending is unset.
        softwareRemovedMatch = std::make_unique<sdbusplus::bus::match_t>(
            bus,
            "type='signal',interface='org.freedesktop.DBus.ObjectManager',"
            "member='InterfacesRemoved',arg0path='" +
                std::string(SOFTWARE_OBJPATH) + "/'",
            [this](sdbusplus::message::message& msg) {
                sdbusplus::object_path path;
                std::vector<std::string> ifaces;
                try
                {
                    msg.read(path, ifaces);
                }
                catch (const std::exception&)
                {
                    return;
                }
                if (detectedPlatformSmcu.empty())
                {
                    return;
                }
                const std::string expected =
                    std::string(SOFTWARE_OBJPATH) + "/" + detectedPlatformSmcu;
                if (std::string(path) == expected)
                {
                    onPlatformChanged();
                }
            });

        // M3: drive one initial convergence so the SMCU object registers on a
        // clean boot even when the daemon is already Ready (signals do not
        // replay, and the base ctor may query inventory paths before
        // detectedPlatformSmcu is set). Idempotent; guarded by
        // onPlatformChanged.
        onPlatformChanged();
    }
    /**
     * @brief Get the Version object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getVersion(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Manufacturer object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string
        getManufacturer(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Model object
     *
     * @param inventoryPath
     * @return std::string
     */
    std::string getModel(const std::string& inventoryPath) const override;

    /**
     * @brief Get the Service Args object
     *
     * @param inventoryPath
     * @param imagePath
     * @param version
     * @param targetFilter
     * @return std::string
     */
    virtual std::string getServiceArgs(
        [[maybe_unused]] const std::string& inventoryPath,
        const std::string& imagePath,
        [[maybe_unused]] const std::string& version,
        [[maybe_unused]] const TargetFilter& targetFilter) const override
    {

        // The systemd unit shall be escaped
        std::string args = inventoryPath;
        args += "\\x20";
        args += imagePath;

        try
        {
            const std::string uuid = std::filesystem::path(imagePath)
                                         .parent_path()
                                         .filename()
                                         .string();
            auto mapSMCUEntry = deviceIds.find(uuid);
            if (mapSMCUEntry != deviceIds.end())
            {
                const std::string& model = std::get<0>(mapSMCUEntry->second);
                if (!model.empty())
                {
                    args += "\\x20";
                    args += model; // pass model name as 3rd arg
                }
            }
        }
        catch (...)
        {
            // Ignore and proceed without third arg
        }
        std::replace(args.begin(), args.end(), '/', '-');

        return args;
    }

    /**
     * @brief Get the Item Updater Inventory Paths object
     *
     * @return std::vector<std::string>
     */

    std::vector<std::string> getItemUpdaterInventoryPaths() override
    {
        std::vector<std::string> ret;
        // If we already processed an image and know the model, use it
        if (currentModelToken == "ASMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
            return ret;
        }
        else if (currentModelToken == "RSMCU")
        {
            ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
            return ret;
        }

        // Priority 2: Use detectedPlatformSmcu (from startup detection)
        if (!detectedPlatformSmcu.empty())
        {
            if (detectedPlatformSmcu == "ASMCU")
            {
                ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/ASMCU");
                return ret;
            }
            else if (detectedPlatformSmcu == "RSMCU")
            {
                ret.emplace_back(std::string(SOFTWARE_OBJPATH) + "/RSMCU");
                return ret;
            }
        }

        // Unknown means register NOTHING; BOTH makes PLDM match both package
        // components and launch jobs that collide on ttyS2 / the mux
        // (bug 6397179 / ARCHITECTURE Sec 10a.1). Self-heal registers the
        // correct single object after the daemon publishes a real identity.
        lg2::error("SMCU type unknown at registration; registering no SMCU "
                   "inventory object (avoids double-flash)");
        return ret; // empty
    }

    bool inventorySupported() override
    {
        return false; // default is supported
    }

    /**
     * @brief Generate stable IDs: ASMCU -> Aurix_SMCU,
     * RSMCU -> Renesas_SMCU; other models fall back to UUID.
     */
    std::string getIdProperty(const std::string& uniqueIdentifier) override
    {
        // Priority 1: Check if called with a UUID (after processImage)
        // Extract model from deviceIds map using the UUID
        if (!uniqueIdentifier.empty())
        {
            auto it = deviceIds.find(uniqueIdentifier);
            if (it != deviceIds.end())
            {
                std::string model = std::get<0>(it->second);
                if (model == "ASMCU")
                {
                    return "ASMCU";
                }
                else if (model == "RSMCU")
                {
                    return "RSMCU";
                }
            }
        }

        // Priority 2: Use cached platform detection (from constructor)
        if (!detectedPlatformSmcu.empty())
        {
            return detectedPlatformSmcu;
        }

        // Priority 3: Detect from the current platform state.
        std::string smcuType = detectPlatformSmcu();
        if (smcuType == "ASMCU" || smcuType == "RSMCU")
        {
            return smcuType;
        }

        // Fallback: preserve the caller's UUID when we cannot derive a stable
        // model-specific ID.
        return uniqueIdentifier.empty() ? getName() : uniqueIdentifier;
    }

    int processImage(std::filesystem::path& filePath) override
    {
        try
        {
            const std::string uuid = filePath.parent_path().filename().string();
            auto it = deviceIds.find(uuid);
            if (it != deviceIds.end())
            {
                currentModelToken = std::get<0>(it->second);
            }
            else
            {
                currentModelToken.clear();
            }
        }
        catch (...)
        {
            currentModelToken.clear();
        }
        return BaseItemUpdater::processImage(filePath);
    }
};

} // namespace updater
} // namespace software
} // namespace nvidia
