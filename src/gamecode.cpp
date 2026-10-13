#include "gamecode.h"
#include "enemy.h"

#include <stdio.h>
#include <string.h>

// Bake-time-linked behavior registry: game code registers into this plain
// table at static init. The table itself is static storage (zero
// initialized), so registration order across translation units doesn't
// matter — no constructor runs before the entries fill in.
static const int MAX_GAME_BEHAVIORS = 16;

struct RegistryEntry
{
    const char* name;
    EnemyBehaviorFn fn;
};
static RegistryEntry registryEntries[MAX_GAME_BEHAVIORS] = {};
static int registryCount = 0;

void gamecodeRegisterEnemyBehavior(const char* name, EnemyBehaviorFn fn)
{
    if (!name || !name[0] || !fn) return;
    for (int i = 0; i < registryCount; i++)
    {
        if (strcmp(registryEntries[i].name, name) == 0)
        {
            if (registryEntries[i].fn == fn) return;  // re-registration
            fprintf(stderr, "warning: game behavior '%s' registered twice"
                    " (keeping the first)\n", name);
            return;
        }
    }
    if (registryCount >= MAX_GAME_BEHAVIORS)
    {
        fprintf(stderr, "warning: game behavior registry full; '%s' ignored\n", name);
        return;
    }
    registryEntries[registryCount].name = name;
    registryEntries[registryCount].fn = fn;
    registryCount++;
}

EnemyBehaviorFn gamecodeFindEnemyBehavior(const char* name)
{
    if (!name || !name[0]) return 0;
    for (int i = 0; i < registryCount; i++)
    {
        if (strcmp(registryEntries[i].name, name) == 0)
            return registryEntries[i].fn;
    }
    return 0;
}
