/*
 * Copyright 2006-2010, Haiku.
 * All rights reserved. Distributed under the terms of the MIT License.
 * Authors:
 *		Adrien Destugues <pulkomandy@gmail.com>
 *		Stephan Aßmus <superstippi@gmx.de>
*/


#ifndef __LANGUAGE_LIST_VIEW_H
#define __LANGUAGE_LIST_VIEW_H


#include <OutlineListView.h>
#include <StringItem.h>
#include <String.h>


class LanguageListItem: public BStringItem {
	public:
		LanguageListItem(const char* text, const char* code);

		LanguageListItem(const LanguageListItem& other)
			:
			BStringItem(other.Text()),
			fLanguageCode(other.fLanguageCode),
			fIcon(other.fIcon)
		{}

		~LanguageListItem();

		const inline BString LanguageCode() { return fLanguageCode; }
		void DrawItem(BView *owner, BRect frame, bool complete = false);

	private:
		const BString fLanguageCode;
		BBitmap* fIcon;
};


static int
compare_list_items(const void* _a, const void* _b)
{
	LanguageListItem* a = *(LanguageListItem**)_a;
	LanguageListItem* b = *(LanguageListItem**)_b;
	return strcasecmp(a->Text(), b->Text());
}


class LanguageListView: public BOutlineListView {
	public:
		LanguageListView(const char* name, list_view_type type);

		~LanguageListView() { delete fMsgPrefLanguagesChanged; }

		bool InitiateDrag(BPoint point, int32 index, bool wasSelected);
		void MouseMoved(BPoint where, uint32 transit, const BMessage* msg);
		void MoveItems(BList& items, int32 index);
		void MoveItemFrom(BOutlineListView* origin, int32 index, int32 dropSpot = 0);
		void AttachedToWindow()
		{
			BOutlineListView::AttachedToWindow();
			ScrollToSelection();
		}
		void MessageReceived (BMessage* message);
	private:
		int32		fDropIndex;
		BMessage*	fMsgPrefLanguagesChanged;
};

#endif