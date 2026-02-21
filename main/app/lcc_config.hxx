/**
 * @file lcc_config.hxx
 * @brief LCC/OpenMRN CDI Configuration Definition for Turnout Panel
 * 
 * Defines the Configuration Description Information (CDI) for this node.
 * The turnout panel configuration includes screen timeout and stale timeout
 * settings configurable via the LCC memory configuration protocol.
 */

#ifndef LCC_CONFIG_HXX_
#define LCC_CONFIG_HXX_

#include "openlcb/ConfigRepresentation.hxx"
#include "openlcb/MemoryConfig.hxx"

namespace openlcb
{

/// Configuration version. Increment when making incompatible changes.
/// v0x0100: Turnout panel — replaces lighting controller config
static constexpr uint16_t CANONICAL_VERSION = 0x0100;

/// Default screen timeout in seconds (0 = disabled)
static constexpr uint16_t DEFAULT_SCREEN_TIMEOUT_SEC = 60;

/// Default stale timeout in seconds (0 = disabled)
static constexpr uint16_t DEFAULT_STALE_TIMEOUT_SEC = 300;

/// Default query pace in milliseconds between state queries
static constexpr uint16_t DEFAULT_QUERY_PACE_MS = 100;

/// CDI segment for panel behavior settings
CDI_GROUP(PanelConfig);

/// Screen timeout for power saving
CDI_GROUP_ENTRY(screen_timeout_sec, Uint16ConfigEntry,
    Name("Screen Backlight Timeout (seconds)"),
    Description("Time in seconds before the screen backlight turns off when idle. "
                "Touch the screen to wake. Set to 0 to disable (always on). "
                "Range: 0 or 10-3600 seconds. Default: 60 seconds."),
    Default(DEFAULT_SCREEN_TIMEOUT_SEC),
    Min(0),
    Max(3600));

/// Stale timeout — mark turnouts as stale if no state update received
CDI_GROUP_ENTRY(stale_timeout_sec, Uint16ConfigEntry,
    Name("Stale Timeout (seconds)"),
    Description("Time in seconds before a turnout is marked STALE if no state "
                "update is received. Set to 0 to disable stale detection. "
                "Default: 300 seconds (5 minutes)."),
    Default(DEFAULT_STALE_TIMEOUT_SEC),
    Min(0),
    Max(3600));

/// Query pace — minimum interval between turnout state queries
CDI_GROUP_ENTRY(query_pace_ms, Uint16ConfigEntry,
    Name("Query Pace (milliseconds)"),
    Description("Minimum interval in milliseconds between turnout state queries "
                "during refresh. Lower values are faster but generate more bus traffic. "
                "Range: 20-1000 ms. Default: 100 ms."),
    Default(DEFAULT_QUERY_PACE_MS),
    Min(20),
    Max(1000));

CDI_GROUP_END();

/// Main CDI segment containing all user-configurable options
/// Laid out at origin 128 to give space for the ACDI user data at the beginning.
CDI_GROUP(LccConfigSegment, Segment(MemoryConfigDefs::SPACE_CONFIG), Offset(128));

/// Internal configuration data (version info for factory reset)
CDI_GROUP_ENTRY(internal_config, InternalConfigData);

/// Panel configuration
CDI_GROUP_ENTRY(panel, PanelConfig, Name("Panel Configuration"));

CDI_GROUP_END();

/// The complete CDI definition for this node
CDI_GROUP(ConfigDef, MainCdi());

/// Standard identification section (populated from SNIP_STATIC_DATA)
CDI_GROUP_ENTRY(ident, Identification);

/// Standard ACDI section
CDI_GROUP_ENTRY(acdi, Acdi);

/// User info segment (standard SNIP user-editable fields)
CDI_GROUP_ENTRY(userinfo, UserInfoSegment);

/// Main configuration segment
CDI_GROUP_ENTRY(seg, LccConfigSegment);

CDI_GROUP_END();

} // namespace openlcb

#endif // LCC_CONFIG_HXX_
