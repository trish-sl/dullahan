/*
    @brief Dullahan - a headless browser rendering engine
           based around the Chromium Embedded Framework
    @author Callum Prentice 2017

    Copyright (c) 2017, Linden Research, Inc.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#define NOMINMAX

#include <algorithm>

#include "wrapper/cef_helpers.h"

#include "dullahan_render_handler.h"

#include "dullahan_impl.h"
#include "dullahan_callback_manager.h"

dullahan_render_handler::dullahan_render_handler(dullahan_impl* parent) :
    mParent(parent)
{
    // inidcates if we should flip the pixel buffer in Y direction
    mFlipYPixels = parent->getFlipPixelsY();

    // indicates if onPageChanged reports only the changed part of the page
    mReportDirtyRects = parent->getReportDirtyRects();

    // the pixel buffer
    mPixelBuffer = nullptr;
    mPixelBufferWidth = 0;
    mPixelBufferHeight = 0;
    mPixelBufferNew = false;

    // the popup buffer
    mPopupBuffer = nullptr;

    // depth is same for all buffer
    mBufferDepth = parent->getDepth();
}

dullahan_render_handler::~dullahan_render_handler()
{
    delete[] mPixelBuffer;

    delete[] mPopupBuffer;
}

void dullahan_render_handler::resizePixelBuffer(int width, int height)
{
    if (mPixelBufferWidth != width || mPixelBufferHeight != height)
    {
        delete[] mPixelBuffer;
        mPixelBufferWidth = width;
        mPixelBufferHeight = height;
        mPixelBuffer = new unsigned char[mPixelBufferWidth * mPixelBufferHeight * mBufferDepth];
        memset(mPixelBuffer, 0xff, mPixelBufferWidth * mPixelBufferHeight * mBufferDepth);

        // the next paint fills the whole buffer, whatever its dirty rectangles say
        mPixelBufferNew = true;
    }
}

// CefRenderHandler override
void dullahan_render_handler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
    CEF_REQUIRE_UI_THREAD();

    int width, height;
    mParent->getSize(width, height);

    resizePixelBuffer(width, height);

    rect = CefRect(0, 0, width, height);
}

void dullahan_render_handler::copyPopupIntoView()
{
    // the popup's top row; in a flipped buffer row y of the view is row (height - 1 - y)
    int popup_y = (mFlipYPixels ? (mPixelBufferHeight - 1 - mPopupBufferRect.y) : mPopupBufferRect.y);
    unsigned char* src = (unsigned char*)mPopupBuffer;
    unsigned char* dst = mPixelBuffer + popup_y * mPixelBufferWidth * mBufferDepth + mPopupBufferRect.x * mBufferDepth;
    while (src < (unsigned char*)mPopupBuffer + mPopupBufferRect.width * mPopupBufferRect.height * mBufferDepth)
    {
        memcpy(dst, src, mPopupBufferRect.width * mBufferDepth);
        src += mPopupBufferRect.width * mBufferDepth;
        dst += mPixelBufferWidth * mBufferDepth * (mFlipYPixels ? -1 : 1);
    }
}

// CefRenderHandler override
void dullahan_render_handler::OnPaint(CefRefPtr<CefBrowser> browser,
                                      PaintElementType type, const RectList& dirtyRects,
                                      const void* buffer, int width, int height)
{
    DLNOUT("onPaint called for size: " << width << " x " << height << " with type: " << type);

    CEF_REQUIRE_UI_THREAD();

    // the part of the page the consumer is told changed, in view coordinates
    CefRect changed(0, 0, mPixelBufferWidth, mPixelBufferHeight);

    // whole page was updated
    if (type == PET_VIEW)
    {
        // create (first time) or resize (browser size changed) a buffer for pixels
        resizePixelBuffer(width, height);
        const bool resized = mPixelBufferNew;
        mPixelBufferNew = false;

        // copy in the union of the dirty rectangles (the whole view after a resize or when
        // dirty rectangles aren't reported), flipped in Y direction as per settings
        CefRect area(0, 0, width, height);
        if (mReportDirtyRects && !resized && !dirtyRects.empty())
        {
            int left = width, top = height, right = 0, bottom = 0;
            for (const CefRect& rect : dirtyRects)
            {
                left = std::min(left, std::max(rect.x, 0));
                top = std::min(top, std::max(rect.y, 0));
                right = std::max(right, std::min(rect.x + rect.width, width));
                bottom = std::max(bottom, std::min(rect.y + rect.height, height));
            }
            area = (right > left && bottom > top) ? CefRect(left, top, right - left, bottom - top) : CefRect(0, 0, 0, 0);
        }
        copyViewRect((const unsigned char*)buffer, area);
        changed = area;

        // if there is still a popup open, write it into the page too (it's pixels will have been
        // copied into it's buffer by a call to OnPaint with type of PET_POPUP earlier)
        if (mPopupBuffer != nullptr)
        {
            copyPopupIntoView();
        }
    }
    // popup was updated
    else if (type == PET_POPUP)
    {
        // copy over the popup pixels into it's buffer
        // (popup buffer created in onPopupSize() as we know the size there)
        memcpy(mPopupBuffer, buffer, width * height * mBufferDepth);

        // copy over popup pixels into page pixels. We need this for when popup is changing (e.g. highlighting or scrolling)
        // when the containing page is not changing and therefore doesn't get an OnPaint update
        copyPopupIntoView();

        const int left = std::max(mPopupBufferRect.x, 0);
        const int top = std::max(mPopupBufferRect.y, 0);
        const int right = std::min(mPopupBufferRect.x + mPopupBufferRect.width, mPixelBufferWidth);
        const int bottom = std::min(mPopupBufferRect.y + mPopupBufferRect.height, mPixelBufferHeight);
        changed = (right > left && bottom > top) ? CefRect(left, top, right - left, bottom - top) : CefRect(0, 0, 0, 0);
    }

    // if we have a buffer, indicate to consuming app that the page changed.
    if (mPixelBufferWidth > 0 && mPixelBufferHeight > 0)
    {
        if (!mReportDirtyRects)
        {
            mParent->getCallbackManager()->onPageChanged(mPixelBuffer, 0, 0, mPixelBufferWidth, mPixelBufferHeight);
        }
        else if (changed.width > 0 && changed.height > 0)
        {
            const CefRect rect = toBufferRect(changed);
            mParent->getCallbackManager()->onPageChanged(mPixelBuffer, rect.x, rect.y, rect.width, rect.height);
        }
    }
}

// Copy a rectangle of the view (CEF's tightly packed buffer, the same size as the pixel
// buffer) into the pixel buffer, flipping it in Y direction as per settings. Each row goes
// straight to its final place, so a flip costs no extra pass over the buffer.
void dullahan_render_handler::copyViewRect(const unsigned char* src, const CefRect& rect)
{
    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }

    const size_t stride = (size_t)mPixelBufferWidth * mBufferDepth;
    const size_t offset = (size_t)rect.x * mBufferDepth;
    const size_t span = (size_t)rect.width * mBufferDepth;
    for (int y = rect.y; y < rect.y + rect.height; ++y)
    {
        const int dst_y = mFlipYPixels ? (mPixelBufferHeight - 1 - y) : y;
        memcpy(mPixelBuffer + dst_y * stride + offset, src + y * stride + offset, span);
    }
}

// A rectangle of the view in pixel buffer coordinates (after any Y flip)
CefRect dullahan_render_handler::toBufferRect(const CefRect& rect) const
{
    if (!mFlipYPixels)
    {
        return rect;
    }
    return CefRect(rect.x, mPixelBufferHeight - (rect.y + rect.height), rect.width, rect.height);
}

// CefRenderHandler override
void dullahan_render_handler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show)
{
    CEF_REQUIRE_UI_THREAD();

    DLNOUT("Popup state set to " << show);
    if (!show)
    {
        delete[] mPopupBuffer;
        mPopupBuffer = nullptr;

        mPopupBufferRect.Set(0,0, 0,0);
    }
}

// CefRenderHandler override
void dullahan_render_handler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect)
{
    CEF_REQUIRE_UI_THREAD();

    mPopupBufferRect = rect;
    if (mPopupBuffer == nullptr)
    {
        mPopupBuffer = new unsigned char[rect.width * rect.height * mBufferDepth];
        memset(mPopupBuffer, 0xff, rect.width * rect.height * mBufferDepth);
    }
}

// CefRenderHandler override
bool dullahan_render_handler::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info)
{
    // this is how the value for "color depth" in the User Agent string is populated
    screen_info.depth = mBufferDepth * 8;

    // indicate we changed the structure
    return true;
}