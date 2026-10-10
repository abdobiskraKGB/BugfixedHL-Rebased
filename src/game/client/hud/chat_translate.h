// chat_translate.h  --  only the declarations. The code is in chat_translate_impl.h
#pragma once

namespace ChatTranslate
{
// Call it for every chat line received from a player (client = index of the player, 0 = server).
void OnChatLine(int client, const char *line, const char *name);

// Call it before sending the text you typed. Returns true when it takes over the sending
// (hud_translate_send is set): the translation is sent when it arrives. Returns false: send as usual.
bool SendTranslated(const char *text, bool team);
}
