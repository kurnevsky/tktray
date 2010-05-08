#include <tcl.h>
#include <tk.h>

#if (!(MAD_TK_PACKAGER))
#include <tkInt.h>
#include <tkIntPlatDecls.h>
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>

#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

/* XEmbed definitions
 * See http://www.freedesktop.org/wiki/Standards_2fxembed_2dspec
 * */
#define XEMBED_MAPPED                   (1 << 0)
/* System tray opcodes
 * See http://www.freedesktop.org/wiki/Standards_2fsystemtray_2dspec
 * */
#define SYSTEM_TRAY_REQUEST_DOCK    0
#define SYSTEM_TRAY_BEGIN_MESSAGE   1
#define SYSTEM_TRAY_CANCEL_MESSAGE  2

/* Flags of widget configuration options */
#define ICON_CONF_IMAGE (1<<0)	    /* Image changed */
#define ICON_CONF_REDISPLAY (1<<1)  /* Redisplay required */
#define ICON_CONF_XEMBED (1<<2)	    /* Remapping or unmapping required */
#define ICON_CONF_CLASS (1<<3)	    /* TODO WM_CLASS update required */
#define ICON_CONF_FIRST_TIME (1<<4) /* For IconConfigureMethod invoked by the constructor */

/* Widget states */
#define ICON_FLAG_REDRAW_PENDING (1<<0)




#if MAD_TK_PACKAGER
static void TKU_WmWithdraw(Tk_Window winPtr)
{
    Tcl_Interp *interp = Tk_Interp(winPtr);
    Tcl_SavedResult saved;
    Tcl_Obj *wm_withdraw;

    Tcl_SaveResult(interp,&saved);
    wm_withdraw = Tcl_NewStringObj("wm withdraw",-1);
    Tcl_IncrRefCount(wm_withdraw);
    Tcl_ListObjAppendElement(NULL,wm_withdraw,Tcl_NewStringObj(Tk_PathName(winPtr),-1));
    Tcl_EvalObj(interp,wm_withdraw);
    Tcl_DecrRefCount(wm_withdraw);
    Tcl_RestoreResult(interp,&saved);
}
/* the wrapper should exist */
static Tk_Window TKU_GetWrapper(Tk_Window winPtr)
{
    Window retRoot, retParent, *retChildren;
    unsigned int nChildren;
    XQueryTree(Tk_Display(winPtr),Tk_WindowId(winPtr),
	       &retRoot,&retParent,&retChildren,&nChildren);
    if (nChildren)
	XFree(retChildren);
    return Tk_IdToWindow(Tk_Display(winPtr), retParent);
}
#else
static void TKU_WmWithdraw(Tk_Window winPtr)
{
    TkpWmSetState((TkWindow*)winPtr, WithdrawnState);
}
static Tk_Window TKU_GetWrapper(Tk_Window winPtr)
{
    return (Tk_Window)
	TkpGetWrapperWindow((TkWindow*)winPtr);
}
#endif

/* Subscribe for extra X11 events (needed for MANAGER selection) */
int TKU_AddInput( Display* dpy, Window win, long add_to_mask)
{
    XWindowAttributes xswa;
    XGetWindowAttributes(dpy,win,&xswa);
    return
	XSelectInput(dpy,win,xswa.your_event_mask|add_to_mask);
}

/* Get Tk Window wrapper (make it exist if ny) */
static Tk_Window TKU_Wrapper(Tk_Window w)
{
    Tk_Window wrapper = TKU_GetWrapper(w);
    if (!wrapper) {
	Tk_MakeWindowExist(w);
	TKU_WmWithdraw(w);
	Tk_MapWindow(w);
	wrapper = TKU_GetWrapper(w);
    }
    return wrapper;
}

/* Return X window id for Tk window (make it exist if ny) */
static Window TKU_XID(Tk_Window w)
{
    Window xid = Tk_WindowId(w);
    if (xid == None) {
	Tk_MakeWindowExist(w);
	xid = Tk_WindowId(w);
    }
    return xid;
}

static void TKU_VirtualEvent(Tk_Window tkwin, Tk_Uid eventid)
{
    union {XEvent general; XVirtualEvent virtual;} event;

    memset(&event, 0, sizeof(event));
    event.general.xany.type = VirtualEvent;
    event.general.xany.serial = NextRequest(Tk_Display(tkwin));
    event.general.xany.send_event = False;
    event.general.xany.window = Tk_WindowId(tkwin);
    event.general.xany.display = Tk_Display(tkwin);
    event.virtual.name = eventid;

    Tk_QueueWindowEvent(&event.general, TCL_QUEUE_TAIL);
}

#define TKU_NO_BAD_WINDOW_BEGIN(display) \
    { Tk_ErrorHandler error__handler = \
	Tk_CreateErrorHandler(display,BadWindow,-1,-1,(int(*)())NULL, NULL);
#define TKU_NO_BAD_WINDOW_END Tk_DeleteErrorHandler(error__handler); }


/* Data structure representing dock widget */
typedef struct {
    /* standard for widget */
    Tk_Window tkwin, drawingWin;
    Window wrapper;
    Window myManager;
    Window trayManager;

    Tk_OptionTable options;
    Tcl_Interp *interp;
    Tcl_Command widgetCmd;

    Tk_Image image; /* image to be drawn */

    Atom aMANAGER;
    Atom a_NET_SYSTEM_TRAY_Sn;
    Atom a_XEMBED_INFO;
    Atom a_NET_SYSTEM_TRAY_MESSAGE_DATA;
    Atom a_NET_SYSTEM_TRAY_OPCODE;
    Atom a_NET_SYSTEM_TRAY_ORIENTATION;

    int flags; /* ICON_FLAG_ - see defines above */
    int msgid; /* Last balloon message ID */
    int useShapeExt;

    int x,y,width,height;
    int imageWidth, imageHeight;
    int requestedWidth, requestedHeight;
    int visible; /* whether XEMBED_MAPPED should be set */
    int docked;	 /* whether an icon should be docked */
    char *imageString, /* option: -image as string */
	*classString; /* option: -class as string */
} DockIcon;


static int TrayIconCreateCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[]);
static int TrayIconObjectCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[]);
static int TrayIconConfigureMethod(DockIcon *icon, Tcl_Interp* interp,
				   int objc, Tcl_Obj* CONST objv[],
				   int addflags);
static int PostBalloon(DockIcon* icon, const char * utf8msg,
		       long timeout);
static void CancelBalloon(DockIcon* icon, int msgid);
static int QueryTrayOrientation(DockIcon* icon);


static void TrayIconDeleteProc( ClientData cd );
static Atom DockSelectionAtomFor(Tk_Window tkwin);
static void DockToManager(DockIcon *icon);
static void CreateTrayIconWindow(DockIcon *icon);

static void TrayIconRequestSize(DockIcon* icon, int w, int h);
static void TrayIconForceImageChange(DockIcon* icon);
static void TrayIconUpdate(DockIcon* icon, int mask);

static void EventuallyRedrawIcon(DockIcon* icon);
static void DisplayIcon(ClientData cd);

static void RetargetEvent(DockIcon *icon, XEvent *ev);

static void TrayIconEvent(ClientData cd, XEvent* ev);
static void UserIconEvent(ClientData cd, XEvent* ev);
static void TrayIconWrapperEvent(ClientData cd, XEvent* ev);
static int IconGenericHandler(ClientData cd, XEvent *ev);

int Tktray_Init ( Tcl_Interp* interp );


static int TrayIconObjectCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[])
{
    DockIcon *icon = (DockIcon*)cd;
    int bbox[4] = {0,0,0,0};
    Tcl_Obj * bboxObj;
    int wcmd;
    int i;
    XWindowAttributes xwa;
    Window bogus;
    int msgid;

    enum {XWC_CONFIGURE=0, XWC_CGET, XWC_BALLOON, XWC_CANCEL, XWC_BBOX, XWC_DOCKED, XWC_ORIENTATION};
    const char *st_wcmd[]={"configure","cget","balloon","cancel","bbox","docked","orientation",NULL};

    long timeout = 0;
    Tcl_Obj* optionValue;

    if (objc<2) {
	Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?args?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], st_wcmd,
			    "subcommand",TCL_EXACT,&wcmd)!=TCL_OK) {
	return TCL_ERROR;
    }

    switch (wcmd) {
    case XWC_CONFIGURE:
	return
	    TrayIconConfigureMethod(icon,interp,objc-2,objv+2,0);
    case XWC_CGET:
	if (objc!=2) {
	    Tcl_WrongNumArgs(interp,1,objv,"option");
	    return TCL_ERROR;
	}
	optionValue = Tk_GetOptionValue(interp,(char*)icon,icon->options,objv[1],icon->tkwin);
	if (optionValue) {
	    Tcl_SetObjResult(interp,optionValue);
	    return TCL_OK;
	} else {
	    return TCL_ERROR;
	}

    case XWC_BALLOON:
	if ((objc!=3) && (objc!=4)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "message ?timeout?");
	    return TCL_ERROR;
	}
	if (objc==4) {
	    if (Tcl_GetLongFromObj(interp,objv[3],&timeout)!=TCL_OK)
		return TCL_ERROR;
	}
	msgid = PostBalloon(icon,Tcl_GetString(objv[2]), timeout);
	Tcl_SetObjResult(interp,Tcl_NewIntObj(msgid));
	return TCL_OK;

    case XWC_CANCEL:
	if (objc!=3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "messageId");
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp,objv[2],&msgid)!=TCL_OK) {
	    return TCL_ERROR;
	}
	if (msgid)
	    CancelBalloon(icon,msgid);
	return TCL_OK;

    case XWC_BBOX:
	if (icon->drawingWin) {
	    XGetWindowAttributes(Tk_Display(icon->drawingWin),
				 TKU_XID(icon->drawingWin),
				 &xwa);

	    XTranslateCoordinates(Tk_Display(icon->drawingWin),
				  TKU_XID(icon->drawingWin),
				  xwa.root, 0,0, &icon->x, &icon->y, &bogus);
	    bbox[0] = icon->x;
	    bbox[1] = icon->y;
	    bbox[2] = bbox[0] + icon->width - 1;
	    bbox[3] = bbox[1] + icon->height - 1;
	}
	bboxObj = Tcl_NewObj();
	for (i=0; i<4; ++i) {
	    Tcl_ListObjAppendElement(interp, bboxObj,
				     Tcl_NewIntObj(bbox[i]));
	}
	Tcl_SetObjResult(interp, bboxObj);
	return TCL_OK;

    case XWC_DOCKED:
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj(icon->myManager!=None));
	return TCL_OK;
    case XWC_ORIENTATION:
	if (icon->myManager == None || icon->wrapper == None) {
	    Tcl_SetResult(interp, "none", TCL_STATIC);
	} else {
	    switch(QueryTrayOrientation(icon)) {
	    case 0:
		Tcl_SetResult(interp, "horizontal", TCL_STATIC); 
		break;
	    case 1:
		Tcl_SetResult(interp, "vertical", TCL_STATIC); 
		break;
	    default:
		Tcl_SetResult(interp, "unknown", TCL_STATIC); 
		break;
	    }
	}
	return TCL_OK;
    }
    return TCL_OK;
}

static int QueryTrayOrientation(DockIcon* icon)
{
    Atom retType = None;
    int retFormat = 32;
    unsigned long retNitems, retBytesAfter;
    unsigned char *retProp = NULL;
    int result=-1;

    if (icon->wrapper != None &&
	icon->myManager != None) {
	XGetWindowProperty(Tk_Display(icon->tkwin),
			   icon->myManager,
			   icon->a_NET_SYSTEM_TRAY_ORIENTATION,
			   /* offset */ 0,
			   /* length */ 1,
			   /* delete */ False,
			   /* type */ XA_CARDINAL,
			   &retType, &retFormat, &retNitems,
			   &retBytesAfter, &retProp);
	if (retType == XA_CARDINAL && retFormat == 32 && retNitems == 1) {
	    result = (int) *(long*)retProp;
	}
	if (retProp) {
	    XFree(retProp);
	}
    }
    return result;
}

static Atom DockSelectionAtomFor(Tk_Window tkwin)
{
    char buf[256];
    /* no snprintf in C89 */
    sprintf(buf,"_NET_SYSTEM_TRAY_S%d",Tk_ScreenNumber(tkwin));
    return Tk_InternAtom(tkwin,buf);
}

static void XembedSetState(DockIcon *icon, long xembedState)
{
    long info[] = { 0, 0 };
    info[1] = xembedState;
    if (icon->drawingWin) {
	XChangeProperty(Tk_Display(icon->drawingWin),
			icon->wrapper,
			icon->a_XEMBED_INFO, 
			icon->a_XEMBED_INFO, 32,
			PropModeReplace, (unsigned char*)info, 2);
    }
}

static void XembedRequestDock(DockIcon *icon)
{
    Tk_Window tkwin = icon->drawingWin;
    XEvent ev;
    Display *dpy = Tk_Display(tkwin);
    
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = icon->myManager;
    ev.xclient.message_type = icon->a_NET_SYSTEM_TRAY_OPCODE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0]=0;
    ev.xclient.data.l[1]=SYSTEM_TRAY_REQUEST_DOCK;
    ev.xclient.data.l[2]=icon->wrapper;
    ev.xclient.data.l[3]=0;
    ev.xclient.data.l[4]=0;
    XSendEvent(dpy, icon->myManager, True, NoEventMask, &ev);
 }

static void CreateTrayIconWindow(DockIcon *icon)
{
    Tcl_SavedResult oldResult;
    Tk_Window tkwin;
    Tk_Window wrapper;
    XSetWindowAttributes attr;
    
    Tcl_SaveResult(icon->interp, &oldResult);
    tkwin = icon->drawingWin = Tk_CreateWindow(icon->interp, icon->tkwin, "inner", "");
    if (tkwin) {
	Tk_SetClass(icon->drawingWin,icon->classString);
	Tk_CreateEventHandler(icon->drawingWin,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|
			      EnterWindowMask|LeaveWindowMask|PointerMotionMask,
			      TrayIconEvent,(ClientData)icon);
	Tk_SetWindowBackgroundPixmap(tkwin, ParentRelative);
	Tk_MakeWindowExist(tkwin);
	TKU_WmWithdraw(tkwin);
	wrapper = TKU_Wrapper(tkwin);

	attr.override_redirect = True;
	Tk_ChangeWindowAttributes(wrapper,CWOverrideRedirect,&attr);
	Tk_CreateEventHandler(wrapper,StructureNotifyMask,TrayIconWrapperEvent,(ClientData)icon);
	Tk_SetWindowBackgroundPixmap(wrapper, ParentRelative);
	icon->wrapper = TKU_XID(wrapper);
	TrayIconForceImageChange(icon);
    } else {
	Tcl_BackgroundError(icon->interp);
    }
    Tcl_RestoreResult(icon->interp, &oldResult);
}

static void DockToManager(DockIcon *icon)
{
    icon->myManager = icon->trayManager;
    TKU_VirtualEvent(icon->tkwin,Tk_GetUid("IconCreate"));
    XembedSetState(icon, icon->visible ? XEMBED_MAPPED : 0);
    XembedRequestDock(icon);
}

static
Tk_OptionSpec IconOptionSpec[]={
    {TK_OPTION_STRING,"-image","image","Image",
     (char *) NULL, -1, Tk_Offset(DockIcon, imageString),
     TK_OPTION_NULL_OK, (ClientData) NULL,
     ICON_CONF_IMAGE | ICON_CONF_REDISPLAY},
    {TK_OPTION_STRING,"-class","class","Class",
     "TrayIcon", -1, Tk_Offset(DockIcon, classString),
     0, (ClientData) NULL,
     ICON_CONF_CLASS},
    {TK_OPTION_BOOLEAN,"-docked","docked","Docked",
     "1", -1, Tk_Offset(DockIcon, docked),
     0, (ClientData) NULL,
     ICON_CONF_XEMBED | ICON_CONF_REDISPLAY},
    {TK_OPTION_BOOLEAN,"-shape","shape","Shape",
     "0", -1, Tk_Offset(DockIcon, useShapeExt),
     0, (ClientData) NULL,
     ICON_CONF_IMAGE | ICON_CONF_REDISPLAY},
    {TK_OPTION_BOOLEAN,"-visible","visible","Visible",
     "1", -1, Tk_Offset(DockIcon, visible),
     0, (ClientData) NULL,
     ICON_CONF_XEMBED | ICON_CONF_REDISPLAY},
    {TK_OPTION_END}
};

static void
TrayIconRequestSize(DockIcon* icon, int w, int h)
{
    if (icon->drawingWin) {
	if (icon->requestedWidth != w ||
	    icon->requestedHeight != h) {
	    Tk_SetMinimumRequestSize(icon->drawingWin,w,h);
	    Tk_GeometryRequest(icon->drawingWin,w,h);
	    Tk_SetGrid(icon->drawingWin,1,1,w,h);
	    icon->requestedWidth = w;
	    icon->requestedHeight = h;
	}
    } else {
	/* Sign that no size is requested yet */
	icon->requestedWidth = 0;
	icon->requestedHeight = 0;
    }
}

static void
TrayIconImageChanged(ClientData cd,
		     int x, int y, int w, int h,
		     int imgw, int imgh)
{
    DockIcon *icon = (DockIcon*) cd;
    icon->imageWidth = imgw;
    icon->imageHeight = imgh;
    TrayIconRequestSize(icon,imgw,imgh);
    EventuallyRedrawIcon(icon);
}

static void TrayIconForceImageChange(DockIcon* icon)
{
    if (icon->image) {
	int w,h;
	Tk_SizeOfImage(icon->image,&w,&h);
	TrayIconImageChanged((ClientData)icon,0,0,w,h,w,h);
    }
}

static void EventuallyRedrawIcon(DockIcon* icon) {
    if (icon->drawingWin && icon->myManager) {	/* don't redraw invisible icon */
	if (!(icon->flags&ICON_FLAG_REDRAW_PENDING)) { /* don't schedule multiple redraw ops */
	    icon->flags|=ICON_FLAG_REDRAW_PENDING;
	    Tcl_DoWhenIdle(DisplayIcon,(ClientData)icon);
	}
    }
}


static void DisplayIcon(ClientData cd)
{
    DockIcon *icon = (DockIcon*)cd;
    int w = icon->imageWidth, h = icon->imageHeight;

    icon->flags&=(~ICON_FLAG_REDRAW_PENDING);

    if (icon->drawingWin) {
	XClearWindow(Tk_Display(icon->drawingWin),
		     TKU_XID(icon->drawingWin));
	if (icon->image && icon->visible) {
	    int imgx, imgy, outx, outy, outw, outh;
	    imgx = (icon->width >= w) ? 0 : -(icon->width - w)/2;
	    imgy = (icon->height >= h) ? 0 : -(icon->height - h)/2;
	    outx = (icon->width >= w) ? (icon->width - w)/2 : 0;
	    outy = (icon->height >= h) ? (icon->height - h)/2 : 0;
	    outw = (icon->width >= w) ? w : icon->width;
	    outh = (icon->height >= h) ? h : icon->width;
	    Tk_RedrawImage(icon->image,imgx,imgy,outw,outh,
			   TKU_XID(icon->drawingWin),
			   outx, outy);
	}
    }
}

static void RetargetEvent(DockIcon *icon, XEvent *ev)
{
    int send = 0;
    Window* saveWin1 = NULL, *saveWin2 = NULL;
    if (!icon->visible)
	return;
    switch (ev->type) {
    case MotionNotify:
	send = 1;
	saveWin1 = &ev->xmotion.subwindow;
	saveWin2 = &ev->xmotion.window;
	break;
    case LeaveNotify:
    case EnterNotify:
	send = 1;
	saveWin1 = &ev->xcrossing.subwindow;
	saveWin2 = &ev->xcrossing.window;
	break;
    case ButtonPress:
    case ButtonRelease:
	send = 1;
	saveWin1 = &ev->xbutton.subwindow;
	saveWin2 = &ev->xbutton.window;
	break;
    case MappingNotify:
	send = 1;
	saveWin1 = &ev->xmapping.window;
    }
    if (saveWin1) {
	Tk_MakeWindowExist(icon->tkwin);
	*saveWin1 = Tk_WindowId(icon->tkwin);
	if (saveWin2) *saveWin2 = *saveWin1;
    }
    if (send) {
	ev->xany.send_event = 0x147321ac;
	Tk_HandleEvent(ev);
    }
}

/* Some embedders, like Docker, add icon windows to save set
   (XAddToSaveSet), so when they crash the icon is reparented to root.
   We have to make sure that automatic mapping in root is done in
   withdrawn state (no way to prevent it entirely)
 */
static void TrayIconWrapperEvent(ClientData cd, XEvent* ev)
{
    DockIcon *icon = (DockIcon*)cd;
    XWindowAttributes attr;
    if (icon->drawingWin) {
	switch(ev->type) {
	case ReparentNotify:
	    /* With virtual roots and screen roots etc, the only way
	       to check for reparent-to-root is to ask for this root
	       first */
	    XGetWindowAttributes(ev->xreparent.display,
				 ev->xreparent.window,
				 &attr);
	    if (attr.root == ev->xreparent.parent) {
		/* upon reparent to root, */
		if (icon->drawingWin) {
		    /* we were sent away to root */
		    TKU_WmWithdraw(icon->drawingWin);
		    if (icon->myManager)
			TKU_VirtualEvent(icon->tkwin,Tk_GetUid("IconDestroy"));
		    icon->myManager = None;

		}
	    } /* Reparenting into some other embedder is theoretically possible,
		 and everything would just work in this case */
	    break;
	}
    }
}

static void TrayIconEvent(ClientData cd, XEvent* ev)
{
    DockIcon *icon = (DockIcon*)cd;

    switch (ev->type) {
    case Expose:
	if (!ev->xexpose.count)
	    EventuallyRedrawIcon(icon);
	break;

    case DestroyNotify:
	/* If anonymous window is destroyed first, then either
	   something went wrong with a tray (if -visible) or we just
	   reconfigured to invisibility: nothing to be done in both
	   cases.
	   If unreal window is destroyed first, freeing the data structures
	   is the only thing to do.
	*/
	if (icon->myManager) {
	    TKU_VirtualEvent(icon->tkwin,Tk_GetUid("IconDestroy"));
	}
	Tcl_CancelIdleCall(DisplayIcon,(ClientData)icon);
	icon->flags &= ~ICON_FLAG_REDRAW_PENDING;
	icon->drawingWin = NULL;
	icon->requestedWidth = 0; /* trigger re-request on recreation */
	icon->requestedHeight = 0;
	icon->wrapper = None;
	icon->myManager = None;
	break;

    case ConfigureNotify:
	TKU_VirtualEvent(icon->tkwin,Tk_GetUid("IconConfigure"));
	if (icon->width != ev->xconfigure.width ||
	    icon->height != ev->xconfigure.height) {
	    icon->width = ev->xconfigure.width;
	    icon->height = ev->xconfigure.height;
	    EventuallyRedrawIcon(icon);
	}
	RetargetEvent(icon,ev);
	break;
    case MotionNotify:
    case ButtonPress: /* fall through */
    case ButtonRelease:
    case EnterNotify:
    case LeaveNotify:
	RetargetEvent(icon,ev);
	break;

    }
}

static void UserIconEvent(ClientData cd, XEvent* ev)
{
    DockIcon *icon = (DockIcon*)cd;

    switch (ev->type) {

    case DestroyNotify:
	Tk_DeleteGenericHandler(IconGenericHandler, (ClientData)icon);
	if(icon->drawingWin) {
	    icon->visible = 0;
	    Tcl_CancelIdleCall(DisplayIcon,(ClientData)icon);
	    icon->flags &= ~ICON_FLAG_REDRAW_PENDING;
	    Tk_DestroyWindow(icon->drawingWin);
	}
	if(icon->image) {
	    Tk_FreeImage(icon->image);
	    icon->image = NULL;
	}
	if(icon->widgetCmd)
	    Tcl_DeleteCommandFromToken(icon->interp,icon->widgetCmd);
	Tk_FreeConfigOptions((char*)icon, icon->options, icon->tkwin);
	break;
    }
}



static int PostBalloon(DockIcon* icon, const char * utf8msg,
		       long timeout)
{
    Tk_Window tkwin = icon -> tkwin;
    Display* dpy = Tk_Display(tkwin);
    int length = strlen(utf8msg);
    XEvent ev;

    if (!(icon->drawingWin) || (icon->myManager == None))
	return 0;
    /* overflow protection */
    if (icon->msgid < 0) 
	icon->msgid = 0;

    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;

    ev.xclient.window = icon->wrapper;

    ev.xclient.message_type =
	icon->a_NET_SYSTEM_TRAY_OPCODE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0]=CurrentTime;
    ev.xclient.data.l[1]=SYSTEM_TRAY_BEGIN_MESSAGE;
    ev.xclient.data.l[2]=timeout;
    ev.xclient.data.l[3]=length;
    ev.xclient.data.l[4]=++icon->msgid;
    TKU_NO_BAD_WINDOW_BEGIN(Tk_Display(icon->tkwin))
	XSendEvent(dpy, icon->myManager , True, NoEventMask, &ev);
    XSync(dpy, False);

    /* Sending message elements */
    ev.xclient.message_type = icon->a_NET_SYSTEM_TRAY_MESSAGE_DATA;
    ev.xclient.format = 8;
    while (length>0) {
	ev.type = ClientMessage;
	ev.xclient.window = icon->wrapper;
	ev.xclient.message_type = icon->a_NET_SYSTEM_TRAY_MESSAGE_DATA;
	ev.xclient.format = 8;
	memset(ev.xclient.data.b,0,20);
	strncpy(ev.xclient.data.b,utf8msg,20);
	XSendEvent(dpy, icon->myManager, True, NoEventMask, &ev);
	XSync(dpy,False);
	utf8msg+=20;
	length-=20;
    }
    TKU_NO_BAD_WINDOW_END;
    return icon->msgid;
}

static void CancelBalloon(DockIcon* icon, int msgid)
{
    Tk_Window tkwin = icon -> tkwin;
    Display* dpy = Tk_Display(tkwin);
    XEvent ev;

    if (!(icon->drawingWin) || (icon->myManager == None))
	return;
    /* overflow protection */
    if (icon->msgid < 0) 
	icon->msgid = 0;

    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;

    ev.xclient.window = icon->wrapper;

    ev.xclient.message_type =
	icon->a_NET_SYSTEM_TRAY_OPCODE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0]=CurrentTime;
    ev.xclient.data.l[1]=SYSTEM_TRAY_CANCEL_MESSAGE;
    ev.xclient.data.l[2]=msgid;
    TKU_NO_BAD_WINDOW_BEGIN(Tk_Display(icon->tkwin))
	XSendEvent(dpy, icon->myManager , True, NoEventMask, &ev);
    TKU_NO_BAD_WINDOW_END
}

/* For non-tk events */
static int IconGenericHandler(ClientData cd, XEvent *ev)
{
    DockIcon *icon = (DockIcon*)cd;

    if ((ev->type == ClientMessage) &&
	(ev->xclient.message_type == icon->aMANAGER) &&
	((Atom)ev->xclient.data.l[1] == icon->a_NET_SYSTEM_TRAY_Sn)) {
	icon->trayManager = (Window)ev->xclient.data.l[2];
	XSelectInput(ev->xclient.display,icon->trayManager,StructureNotifyMask);
	if (icon->myManager == None)
	    TrayIconUpdate(icon, ICON_CONF_XEMBED);
	return 1;
    }
    if (ev->type == DestroyNotify) {
	if (ev->xdestroywindow.window == icon->trayManager) {
	    icon->trayManager = None;
	}
	if (ev->xdestroywindow.window == icon->myManager) {
	    icon->myManager = None;
	    icon->wrapper = None;
	    if (icon->drawingWin) {
		Tk_DestroyWindow(icon->drawingWin);
		icon->drawingWin = NULL;
	    }
	}
    }
    return 0;
}

/* Get in touch with new options that are certainly valid */
static void TrayIconUpdate(DockIcon* icon, int mask)
{
    /* why should someone need this option?
       anyway, let's handle it if we provide it */
    if (mask & ICON_CONF_CLASS) {
	if (icon->drawingWin)
	    Tk_SetClass(icon->drawingWin,Tk_GetUid(icon->classString));
    }
    /*
       First, ensure right icon visibility.
       If should be visible and not yet managed,
       we have to get the tray or wait for it.
       If should be invisible and managed,
       real-window is simply destroyed.
       If should be invisible and not managed,
       generic handler should be abandoned.
    */
    if (mask & ICON_CONF_XEMBED) {
	if (icon->myManager == None &&
	    icon->trayManager != None && 
	    icon->docked) {
	    if (!icon->drawingWin) {
		CreateTrayIconWindow(icon);
	    }
	    if (icon->drawingWin) {
		DockToManager(icon);
	    }
	}
	if (icon->myManager != None &&
	    icon->drawingWin != NULL &&
	    !icon->docked) {
	    Tk_DestroyWindow(icon->drawingWin);
	    icon->drawingWin = NULL;
	    icon->myManager = None;
	    icon->wrapper = None;
	}
	if (icon->drawingWin) {
	    XembedSetState(icon, icon->visible ? XEMBED_MAPPED : 0);
	}
    }
    if (mask & ICON_CONF_IMAGE) {
	TrayIconForceImageChange(icon);
    }
    if (mask & ICON_CONF_REDISPLAY) {
	EventuallyRedrawIcon(icon);
    }
}

/* return TCL_ERROR if some option is invalid,
   or else retrieve resource references and free old resources
*/
static int TrayIconConfigureMethod(DockIcon *icon, Tcl_Interp* interp,
				   int objc, Tcl_Obj* CONST objv[],
				   int addflags)
{
    Tk_SavedOptions saved;
    Tk_Image newImage = NULL;
    int mask = 0;

    if (objc<=1 && !(addflags & ICON_CONF_FIRST_TIME)) {
	Tcl_Obj* info = Tk_GetOptionInfo(interp,(char*)icon,icon->options,
					 objc? objv[0]: NULL, icon->tkwin);
	if (info) {
	    Tcl_SetObjResult(interp,info);
	    return TCL_OK;
	} else {
	    return TCL_ERROR; /* msg by Tk_GetOptionInfo */
	}
    }

    if (Tk_SetOptions(interp,(char*)icon,icon->options,objc,objv,
		      icon->tkwin,&saved,&mask)!=TCL_OK) {
	return TCL_ERROR; /* msg by Tk_SetOptions */
    }
    mask |= addflags;
    /* now check option validity */
    if (mask & ICON_CONF_IMAGE) {
	if (icon->imageString) {
	    newImage = Tk_GetImage(interp, icon->tkwin, icon->imageString,
				   TrayIconImageChanged, (ClientData)icon);
	    if (!newImage) {
		Tk_RestoreSavedOptions(&saved);
		return TCL_ERROR; /* msg by Tk_GetImage */
	    }
	}
	if (icon->image) {
	    Tk_FreeImage(icon->image);
	}
	icon->image = newImage; /* may be null, as intended */
    }
    Tk_FreeSavedOptions(&saved);
    /* Now as we are reconfigured... */
    TrayIconUpdate(icon,mask);
    return TCL_OK;
}

static void TrayIconDeleteProc( ClientData cd )
{
    DockIcon *icon = (DockIcon*) cd;
    Tk_DestroyWindow(icon->tkwin);
}

/*
  Create tray command and (unreal) window.
*/

static int TrayIconCreateCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[])
{
    DockIcon *icon;

    icon = (DockIcon*)attemptckalloc(sizeof(DockIcon));
    if (!icon) {
	Tcl_SetResult(interp, "running out of memory", TCL_STATIC);
	goto handleErrors;
    }
    memset(icon,0,sizeof(*icon));

    if (objc < 2||(objc%2)) {
	Tcl_WrongNumArgs(interp, 1, objv, "pathName ?option value ...?");
	goto handleErrors;
    }

    /* It's not a toplevel window by now. It really doesn't matter,
       because it's not really shown */
    icon->tkwin =
	Tk_CreateWindowFromPath(interp,
				Tk_MainWindow(interp),
				Tcl_GetString(objv[1]),"");
    if (icon->tkwin == NULL) {
	goto handleErrors;
    }

    /* Subscribe to StructureNotify */
    TKU_AddInput(Tk_Display(icon->tkwin),
		 RootWindowOfScreen(Tk_Screen(icon->tkwin)),StructureNotifyMask);
    TKU_AddInput(Tk_Display(icon->tkwin),
		 RootWindow(Tk_Display(icon->tkwin),0),StructureNotifyMask);
    /* Spec says "screen 0" not "default", but... */
    TKU_AddInput(Tk_Display(icon->tkwin),
		 DefaultRootWindow(Tk_Display(icon->tkwin)),StructureNotifyMask);

    /* Early tracking of DestroyNotify is essential */
    Tk_CreateEventHandler(icon->tkwin,StructureNotifyMask,
			  UserIconEvent,(ClientData)icon);

    /* Now try setting options */
    icon->options = Tk_CreateOptionTable(interp,IconOptionSpec);
    /* Class name is used for retrieving defaults, so... */
    Tk_SetClass(icon->tkwin, Tk_GetUid("TrayIcon"));
    if (Tk_InitOptions(interp,(char*)icon,icon->options,icon->tkwin)!=TCL_OK) {
	goto handleErrors;
    }

    icon->a_NET_SYSTEM_TRAY_Sn = DockSelectionAtomFor(icon->tkwin);
    icon->a_NET_SYSTEM_TRAY_OPCODE = Tk_InternAtom(icon->tkwin,"_NET_SYSTEM_TRAY_OPCODE");
    icon->a_NET_SYSTEM_TRAY_MESSAGE_DATA = Tk_InternAtom(icon->tkwin,"_NET_SYSTEM_TRAY_MESSAGE_DATA");
    icon->a_NET_SYSTEM_TRAY_ORIENTATION = Tk_InternAtom(icon->tkwin,"_NET_SYSTEM_TRAY_ORIENTATION");
    icon->a_XEMBED_INFO = Tk_InternAtom(icon->tkwin,"_XEMBED_INFO");
    icon->aMANAGER = Tk_InternAtom(icon->tkwin,"MANAGER");
    icon->interp = interp;

    icon->trayManager = XGetSelectionOwner(Tk_Display(icon->tkwin),icon->a_NET_SYSTEM_TRAY_Sn);
    if (icon->trayManager) {
	Tk_CreateErrorHandler(Tk_Display(icon->tkwin),BadWindow,-1,-1, NULL, NULL);
	XSelectInput(Tk_Display(icon->tkwin),icon->trayManager, StructureNotifyMask);
    }

    Tk_CreateGenericHandler(IconGenericHandler, (ClientData)icon);

    if (objc>3) {
	if (TrayIconConfigureMethod(icon, interp, objc-2, objv+2,
				    ICON_CONF_XEMBED|ICON_CONF_IMAGE|ICON_CONF_FIRST_TIME)!=TCL_OK) {
	    goto handleErrors;
	}
    }

    icon->widgetCmd =
	Tcl_CreateObjCommand(interp, Tcl_GetString(objv[1]),
			     TrayIconObjectCmd, (ClientData)icon, TrayIconDeleteProc);


    /* Sometimes a command just can't be created... */
    if (!icon->widgetCmd) {
	goto handleErrors;
    }

    return TCL_OK;

handleErrors:
    /* Rolling back */
    if (icon) {
	if (icon->options) {
	    Tk_DeleteOptionTable(icon->options);
	    icon->options = NULL;
	}
	if (icon->tkwin) {
	    /* Resources will be freed by DestroyNotify handler */
	    Tk_DestroyWindow(icon->tkwin);
	}
	ckfree((char*)icon);
    }
    return TCL_ERROR;
}


int Tktray_Init ( Tcl_Interp* interp )
{
    if (Tcl_InitStubs( interp, "8.4", 0) == NULL)
	return TCL_ERROR;
    if (Tk_InitStubs( interp, "8.4", 0) == NULL)
	return TCL_ERROR;

    Tcl_CreateObjCommand(interp, "::tktray::icon",
			 TrayIconCreateCmd, NULL, NULL );

    Tcl_PkgProvide( interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}
