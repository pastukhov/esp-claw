cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/../../components/claw_modules"
    # Only include the specific capabilities rover_s3 actually uses.
    # Rover movement/gripper (cap_rover) and camera (cap_unitv) are not
    # included here: that hardware is now driven through an external MCP
    # server instead of this firmware's own I2C/UART drivers.
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_voice"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_mcp_client"
    "${CMAKE_SOURCE_DIR}/../../components/common/mcp_mdns"
    # cap_im_platform/cap_files/cap_skill_mgr/cap_session_mgr/cap_system are
    # not used directly by rover_s3's own sources; app_claw's own manifest
    # (components/common/app_claw/idf_component.yml) pulls them in
    # conditionally based on the CONFIG_APP_CLAW_CAP_* flags in
    # sdkconfig.defaults.
    # Common utilities (only what rover_s3 actually needs)
    "${CMAKE_SOURCE_DIR}/../../components/common/http_reuse"
    "${CMAKE_SOURCE_DIR}/../../components/common/captive_dns"
    "${CMAKE_SOURCE_DIR}/../../components/common/app_claw"
    "${CMAKE_SOURCE_DIR}/components"
)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_s3)
