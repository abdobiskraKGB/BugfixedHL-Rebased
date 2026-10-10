// chat_translate.h  --  only the declaration. The code is in chat_translate_impl.h
#pragma once

namespace ChatTranslate
{
// Call it for every chat line received from a player (client = index of the player, 0 = server).
void OnChatLine(int client, const char *line, const char *name);
}
