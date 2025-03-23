#include <base/color.h>
#include <base/system.h>

#include "teeinfo.h"

CTeeInfo::CTeeInfo(const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet)
{
	str_copy(m_aSkinName, pSkinName, sizeof(m_aSkinName));
	m_UseCustomColor = UseCustomColor;
	m_ColorBody = ColorBody;
	m_ColorFeet = ColorFeet;
}
