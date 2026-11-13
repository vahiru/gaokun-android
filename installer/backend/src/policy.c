/* 安装前门槛（说明见 policy.h）。 */
#include "policy.h"

#include <string.h>

int gk3w_model_supported(const char *product_name)
{
    static const char *const models[] = {"GK-W7X"};
    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++)
        if (!strcmp(product_name, models[i]))
            return 1;
    return 0;
}

int gk3w_battery_low(int ac, int battery)
{
    return ac == 0 && battery >= 0 && battery < GK3W_BATTERY_MIN_PCT;
}
