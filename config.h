#ifndef CONFIG_H
#define CONFIG_H

#define API_URL       "https://n8nflow.duckdns.org/webhook/21478d4c-27c4-4c74-9ac9-def4ee246fb5"
#define API_URL_META  API_URL "?type=meta"
#define API_URL_IMAGE API_URL "?type=image"
#define API_URL_DIFF  API_URL "?type=imageDiff"
#define API_URL_REGION API_URL "?type=imageRegion"

#define FULL_FETCH_INTERVAL_HOURS 4

#define REFRESH_INTERVAL_MINUTES 1

#define REFRESH_INTERVAL_MIN_SECONDS 60
#define REFRESH_INTERVAL_MAX_SECONDS 14400  // 4 hours

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480

#endif
