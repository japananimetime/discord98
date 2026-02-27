#pragma once

#include "Main.hpp"
#include <vector>
#include <string>

struct ScreenSource
{
	enum Type { Monitor, Window };

	Type type;
	int adapterIndex;  // for monitors
	int outputIndex;   // for monitors
	HWND hwnd;         // for windows
	std::string title;
	HBITMAP hThumbnail; // preview thumbnail (owned, must be deleted)

	ScreenSource() : type(Monitor), adapterIndex(0), outputIndex(0), hwnd(NULL), hThumbnail(NULL) {}
	~ScreenSource() { if (hThumbnail) DeleteObject(hThumbnail); }

	// Non-copyable, movable
	ScreenSource(const ScreenSource&) = delete;
	ScreenSource& operator=(const ScreenSource&) = delete;
	ScreenSource(ScreenSource&& o) noexcept
		: type(o.type), adapterIndex(o.adapterIndex), outputIndex(o.outputIndex),
		  hwnd(o.hwnd), title(std::move(o.title)), hThumbnail(o.hThumbnail)
	{
		o.hThumbnail = NULL;
	}
	ScreenSource& operator=(ScreenSource&& o) noexcept
	{
		if (this != &o) {
			if (hThumbnail) DeleteObject(hThumbnail);
			type = o.type; adapterIndex = o.adapterIndex; outputIndex = o.outputIndex;
			hwnd = o.hwnd; title = std::move(o.title); hThumbnail = o.hThumbnail;
			o.hThumbnail = NULL;
		}
		return *this;
	}
};

struct ScreenPickerResult
{
	bool useWindow = false;
	int adapterIndex = 0;
	int outputIndex = 0;
	HWND hwnd = NULL;
};

// Shows modal screen picker dialog. Returns true if user selected a source.
bool ShowScreenPickerDialog(HWND hwndParent, ScreenPickerResult& result);
