#pragma once

#include <cstdint>

struct IpcTileTypeEntry;

namespace IpcTileState {

bool IsWalkable(float worldX, float worldY);
void GetStats(int* outTileCount, int* outNoWalkTypeCount);
int CopyUniqueTypeEntries(IpcTileTypeEntry* buf, int maxCount);
void ClearTiles();
void InitNoWalkTypes(char* typesBuf);
void ApplyTileUpdate(char* tilesBuf);

} // namespace IpcTileState
