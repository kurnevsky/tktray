#include <tcl.h>
#include <tk.h>
#include <tkInt.h>
#include <tkIntPlatDecls.h>
#include <string.h>
#include <stdio.h>

#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/Xmd.h>
#include <X11/extensions/shape.h>

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
#define ICON_CONF_IMAGE (1<<0)	    // Image changed
#define ICON_CONF_REDISPLAY (1<<1)  // Redisplay required
#define ICON_CONF_XEMBED (1<<2)	    // Update _XEMBED_INFO 
#define ICON_CONF_CLASS (1<<3)	    // Update WM_CLASS

/* Widget states */
#define ICON_FLAG_REDRAW_PENDING (1<<0)
#define ICON_FLAG_DELETED (1<<1)
#define ICON_FLAG_MANAGED (1<<2)
#define ICON_FLAG_RESHAPE_ON_REDRAW (1<<3)


/* Image representation utilities
 * TKU_ImageRep represents Tk image, 
 * contains "mask" pixmap that is updated
 * from the image transparency info
 * */
typedef struct {
    Tk_Window tkwin;
    Tk_Image tkimg;
    Tcl_Interp *interp;
    char *image_name;
    Pixmap mask; 
    ClientData client_data;
    Tk_ImageChangedProc *changed_proc;
    int w, h;
    int resized;
} TKU_ImageRep;

static void
TKU_InitImageRep ( TKU_ImageRep * ir )
{
    memset(ir, 0, sizeof(TKU_ImageRep));
}

static void 
TKU_CleanupImageRep( TKU_ImageRep * ir )
{
    if (ir->tkimg != NULL) {
	Tk_FreeImage(ir->tkimg);
	ir->image_name = NULL;
    }
    if (ir->mask != None) {
	Tk_FreePixmap(Tk_Display(ir->tkwin), ir->mask);
	ir->mask = None;
    }
}

static void
TKU_ImageChanged(ClientData cd,int x, int y, int w, int h, int imgw, int imgh)
{
    TKU_ImageRep *ir = (TKU_ImageRep*) cd;
    ir->resized = ( (ir->w != imgw) || (ir->h != imgh) 
	    ||(ir->mask == None) );
    if (ir->resized) {
	Tk_MakeWindowExist(ir->tkwin);
	ir->w = imgw;
	ir->h = imgh;
	if (ir->mask != None) {
	    Tk_FreePixmap(Tk_Display(ir->tkwin), ir->mask);
	    ir->mask = None;
	}
	if (! Tk_WindowId(ir->tkwin))
	    return;
	ir->mask = Tk_GetPixmap(
		Tk_Display(ir->tkwin),
		Tk_WindowId(ir->tkwin),
		imgw, imgh, 1);
	x = 0; y = 0; w = imgw; h = imgh;
    }
    Tk_PhotoHandle photo;
    photo = Tk_FindPhoto(ir->interp, ir->image_name);
    if (!photo) return;
    Tk_PhotoImageBlock pib;
    Tk_PhotoGetImage(photo,&pib);
    /* Allright: create XImage, put it onto mask */
    char *ida = Tcl_Alloc(w*h);
    XImage *xim = XCreateImage(Tk_Display(ir->tkwin),
	    Tk_Visual(ir->tkwin),1,XYBitmap,0,ida,w,h,8,0); 
    register int cx,cy;
    for(cy=0;cy<h;++cy)
	for(cx=0;cx<w;++cx) {
	    unsigned char alpha = 
		pib.pixelPtr [ cx*pib.pixelSize + 
		cy*pib.pitch + pib.offset[3] ];
	    XPutPixel(xim, cx, cy, alpha<128? 0: 1);
	}

    XGCValues gcv;
    gcv.foreground = 1;
    gcv.background = 0;
    GC gc = XCreateGC(Tk_Display(ir->tkwin),
	    ir->mask,GCForeground|GCBackground,&gcv);
    XSync(Tk_Display(ir->tkwin),False); //FIXME: DEBUG:
    XPutImage(Tk_Display(ir->tkwin),ir->mask,gc,xim,0,0,x,y,w,h);
    XFreeGC(Tk_Display(ir->tkwin),gc);
    Tcl_Free(ida);
    xim->data = NULL;
    XDestroyImage(xim);
    if (ir->changed_proc != NULL) {
	ir->changed_proc(ir->client_data,x,y,w,h,imgw,imgh);
    }
}

static int
TKU_BindImageRep(Tcl_Interp *interp, Tk_Window tkwin,
	char *image_name, TKU_ImageRep *ir) {
    Tk_PhotoHandle photo;
    if (image_name == NULL)
	return TCL_ERROR;
    photo = Tk_FindPhoto(interp, image_name);
    if (!photo) {
	return TCL_ERROR;
    }
    TKU_CleanupImageRep(ir);
    ir->interp = interp;
    ir->image_name = image_name;
    ir->tkwin = tkwin;
    ir->tkimg = Tk_GetImage(interp, tkwin, image_name, TKU_ImageChanged,
	    (ClientData)ir);
    if (ir->tkimg == NULL) {
	return TCL_ERROR;
    }
    int w,h;
    Tk_PhotoGetSize(photo,&w,&h);
    TKU_ImageChanged((ClientData)ir,0,0,w,h,w,h);
    return TCL_OK;
}


/* Make sure Tk wrapper exists for the toplevel. 
 * Map window in Withdrawn state, so it remains invisible.
 * */
static void 
TKU_CompleteToplevel(Tk_Window tkwin) 
{
    if (!Tk_IsMapped(tkwin))
	Tk_MapWindow(tkwin);
}

/* Set some new bits in the window's event mask. */
static int 
TKU_AddInput( Display* dpy, Window win, long add_to_mask)
{
    XWindowAttributes xswa;
    XGetWindowAttributes(dpy,win,&xswa);
    return
	XSelectInput(dpy,win,xswa.your_event_mask|add_to_mask);
}

/* Get X11 window's parent */
static Window TKU_Parent(Display *dpy, Window w)
{
    Window root,parent=None,*children=NULL;
    unsigned int nchildren;
    XQueryTree(dpy,w,&root,&parent,&children,&nchildren);
    if (children) XFree(children);
    return parent;
}


/* Data structure representing dock widget */
typedef struct {
    /* standard for widget */
    Tk_Window tkwin;
    Window wrapper;
    Window root;
    Tk_OptionTable options;
    Tcl_Interp *interp;
    Tcl_Command widgetCmd;

    TKU_ImageRep img; /* image to be drawn */
    Window tray; /* system tray handling the icon */
    Atom docksel; /* _NET_SYSTEM_TRAY_Sn */
    int flags; /* ICON_FLAG_ - see defines above */
    int msgid; /* Last balloon message ID */

    int visible; /* XEMBED_MAPPED */
    char *imageString, // option: -image as string
	 *classString; // option: -class as string
} DockIcon;

static void EventuallyRedrawIcon(DockIcon* icon);
static void DisplayIcon(ClientData cd);
static int IconGenericHandler(ClientData cd, XEvent *ev);
static void IconEvent(ClientData cd, XEvent* ev);
static void WatchTrayManagerSel(DockIcon* icon);
static int PostBalloon(DockIcon* icon, const char * utf8msg, 
	long timeout);
static int TrayIconCreateCmd(ClientData cd, Tcl_Interp *interp,
	int objc, Tcl_Obj * CONST objv[]);
static int TrayIconObjectCmd(ClientData cd, Tcl_Interp *interp,
	int objc, Tcl_Obj * CONST objv[]);
static void TrayIconDeleteProc( ClientData cd );
static void IconDestroy(char* cd);
static Atom DockSelectionAtomFor(Tk_Window tkwin);
static void TryDock(DockIcon *icon);
static int UpdateIcon( DockIcon* icon, int mask);
static int ConfigureIcon(DockIcon * icon,
	Tcl_Interp * interp,
	int objc, Tcl_Obj * CONST objv[]);
static void IconImageChanged(ClientData , int x, int y, int w, int h, 
	int imgw, int imgh);

int Tktray_Init ( Tcl_Interp* interp );


static int TrayIconObjectCmd(ClientData cd, Tcl_Interp *interp,
	int objc, Tcl_Obj * CONST objv[]) 
{
    DockIcon *icon = (DockIcon*)cd;
    int bbox[4];
    Tcl_Obj * bboxObj;
    Window child_return;
    int wcmd;
    int i;
    enum {XWC_CONFIGURE=0, XWC_BALLOON, XWC_BBOX};
    const char *st_wcmd[]={"configure","balloon","bbox",NULL};

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
		ConfigureIcon(icon, interp, objc-2, objv+2);
	case XWC_BALLOON:
	    /* FIXME: not implemented yet */
	    if ((objc!=3) && (objc!=4)) {
		Tcl_WrongNumArgs(interp, 2, objv, "message ?timeout?");
		return TCL_ERROR;
	    }
	    long timeout = 0;
	    if (objc==4) 
		if (Tcl_GetLongFromObj(interp,objv[3],&timeout)!=TCL_OK)
		    return TCL_ERROR;
	    PostBalloon(icon,Tcl_GetString(objv[2]), timeout);
	    return TCL_OK;
	case XWC_BBOX:
	    if (XTranslateCoordinates(Tk_Display(icon->tkwin),Tk_WindowId(icon->tkwin),
		    icon->root, 0, 0, &bbox[0], &bbox[1], &child_return) ) {
		bbox [2] = bbox[0]+Tk_Width(icon->tkwin);
		bbox [3] = bbox[1]+Tk_Height(icon->tkwin);
		bboxObj = Tcl_NewObj();
		for (i=0; i<4; ++i) {
		    Tcl_ListObjAppendElement(interp, bboxObj, 
			    Tcl_NewIntObj(bbox[i]));
		}
		Tcl_SetObjResult(interp, bboxObj);
	    }
    }
    return TCL_OK;
}

static void TrayIconDeleteProc( ClientData cd )
{
    DockIcon *icon = (DockIcon*)cd;
    if (!(icon->flags & ICON_FLAG_DELETED)) {
	Tk_DestroyWindow(icon->tkwin);
    }
}

static void IconDestroy(char* cd)
{
    DockIcon *icon = (DockIcon*)cd;
    Tk_DeleteGenericHandler(IconGenericHandler, cd);
    TKU_CleanupImageRep(&icon->img);
    Tcl_DeleteCommandFromToken(icon->interp,icon->widgetCmd);
    Tk_FreeConfigOptions((char*)icon,icon->options,icon->tkwin);
    Tcl_Free((char*)icon);
}

static Atom DockSelectionAtomFor(Tk_Window tkwin) 
{
    char buf[256];
    snprintf(buf,sizeof(buf),"_NET_SYSTEM_TRAY_S%d",Tk_ScreenNumber(tkwin));
    return Tk_InternAtom(tkwin,buf);
}

static void TryDock(DockIcon *icon) 
{
    Tk_Window tkwin = icon -> tkwin;
    Display *dpy = Tk_Display(tkwin);
    Window tray =
	XGetSelectionOwner(dpy,icon->docksel);
    if (tray != None) {
	Tk_MakeWindowExist(tkwin);
	XEvent ev;
    	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = tray;
	ev.xclient.message_type = 
	    Tk_InternAtom(tkwin,"_NET_SYSTEM_TRAY_OPCODE");
	ev.xclient.format = 32;
	ev.xclient.data.l[0]=0;
	ev.xclient.data.l[1]=SYSTEM_TRAY_REQUEST_DOCK;
	ev.xclient.data.l[2]= icon->wrapper;  // Tk_WindowId(tkwin);
	ev.xclient.data.l[3]=0;
	ev.xclient.data.l[4]=0;
	XSendEvent(dpy, tray, False, NoEventMask, &ev);
	XSync(dpy, False);
	icon -> tray = tray;
	icon -> flags |= ICON_FLAG_MANAGED;
	/* FIXME: potential race, fatal */
    } else {
	WatchTrayManagerSel(icon);
    }
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
    {TK_OPTION_BOOLEAN,"-visible","visible","Visible",
	"1", -1, Tk_Offset(DockIcon, visible), 
	0, (ClientData) NULL,
	ICON_CONF_XEMBED},
    {TK_OPTION_END}
};


static int ConfigureIcon(DockIcon * icon,
	Tcl_Interp * interp,
	int objc, Tcl_Obj * CONST objv[]) 
{
    int mask=0;
    Tk_SavedOptions saved;
    if (Tk_SetOptions(interp, (char*)icon, icon->options, objc, objv, icon->tkwin, 
	    &saved, &mask)!=TCL_OK) {
	return TCL_ERROR;
    }
    if (UpdateIcon(icon,mask)!=TCL_OK) {
	Tk_RestoreSavedOptions(&saved);
	return TCL_ERROR;
    } else {
	Tk_FreeSavedOptions(&saved);
	return TCL_OK;
    }
}

static int UpdateIcon( DockIcon* icon, int mask) 
{
    Tk_Window tkwin = icon -> tkwin;
    Window w = icon -> wrapper;
    Display *dpy = Tk_Display(tkwin);
    Tk_MakeWindowExist(tkwin);
    Atom XEMBED_INFO = Tk_InternAtom(tkwin,"_XEMBED_INFO");
    if (mask & ICON_CONF_IMAGE) {
	if (TKU_BindImageRep(icon->interp,tkwin,
			     icon->imageString,&icon->img) != TCL_OK)
	    return TCL_ERROR;
    }
    if (mask & ICON_CONF_CLASS) {
	if (icon->classString!=NULL)
	    Tk_SetClass(tkwin,icon->classString);
    }
    if (mask & ICON_CONF_XEMBED) {
	long data[2]={0,0};
	data[1]=icon->visible? XEMBED_MAPPED: 0;
	XChangeProperty(dpy, w, XEMBED_INFO, XEMBED_INFO, 32, 
		PropModeReplace, (unsigned char*)data, 2);
    }
    if (mask & ICON_CONF_REDISPLAY) {
	EventuallyRedrawIcon(icon);
    }
    return TCL_OK;
}

static void
IconImageChanged(ClientData cd, int x, int y, int w, int h, int imgw, int imgh)
{
    DockIcon *icon = (DockIcon*) cd;
    icon->flags |=ICON_FLAG_RESHAPE_ON_REDRAW;
    if (icon->img.resized) {
	Tk_MakeWindowExist(icon->tkwin);
	XSizeHints *xszh = XAllocSizeHints();
	xszh->flags = PMinSize;
	xszh->min_width = imgw+2;
	xszh->min_height = imgh+2;
	XSetWMNormalHints(Tk_Display(icon->tkwin),
		icon->wrapper, xszh);
    }
    EventuallyRedrawIcon(icon);
}


static 
void EventuallyRedrawIcon(DockIcon* icon) {
    if (!(icon->flags&ICON_FLAG_REDRAW_PENDING)) {
	icon->flags|=ICON_FLAG_REDRAW_PENDING;
	Tcl_DoWhenIdle(DisplayIcon,(ClientData)icon);
    }
}

static 
void WatchTrayManagerSel(DockIcon* icon) {
    Tk_Window tkwin = icon -> tkwin;
    TKU_AddInput(Tk_Display(tkwin),
	    RootWindowOfScreen(Tk_Screen(tkwin)),StructureNotifyMask);
    TKU_AddInput(Tk_Display(tkwin),
	    DefaultRootWindow(Tk_Display(tkwin)),StructureNotifyMask);
    Tk_CreateGenericHandler(IconGenericHandler, (ClientData)icon);
}

static int
IconGenericHandler(ClientData cd, XEvent *ev)
{
    DockIcon *icon = (DockIcon*)cd;
    if (icon->flags & ICON_FLAG_DELETED ||
	    icon->flags & ICON_FLAG_MANAGED) {
	Tk_DeleteGenericHandler(IconGenericHandler, cd);
	return 0;
    }
    if (ev->type != ClientMessage)
	return 0;
    if (ev->xclient.message_type != Tk_InternAtom(icon->tkwin,"MANAGER"))
	return 0;
    if ((Atom)ev->xclient.data.l[1] != icon->docksel)
	return 0;

    Tk_DeleteGenericHandler(IconGenericHandler, cd);
    Tcl_DoWhenIdle((Tcl_IdleProc*)TryDock, cd);
    return 1;
}


static
void DisplayIcon(ClientData cd)
{
    DockIcon *icon = (DockIcon*)cd;
    if (icon->flags & ICON_FLAG_DELETED) return ;
    int winw = Tk_Width(icon->tkwin),
	winh = Tk_Height(icon->tkwin);
    icon->flags&=(~ICON_FLAG_REDRAW_PENDING);
    if (winw==0||winh==0||(!icon->visible)||
	    icon->img.tkimg==NULL)
	return;

    if (icon->flags & ICON_FLAG_RESHAPE_ON_REDRAW) {
	icon->flags &= (~ICON_FLAG_RESHAPE_ON_REDRAW);
	XShapeCombineMask(Tk_Display(icon->tkwin),
		icon->wrapper, ShapeBounding,
		(winw - icon->img.w)/2,
		(winh - icon->img.h)/2, 
		icon->img.mask,ShapeSet);
    }
    Tk_RedrawImage(icon->img.tkimg,0,0,icon->img.w,icon->img.h,
	    Tk_WindowId(icon->tkwin),
	    (winw - icon->img.w)/2,
	    (winh - icon->img.h)/2);
}

static void IconEvent(ClientData cd, XEvent* ev)
{
    DockIcon *icon = (DockIcon*)cd;
    switch (ev->type) {
	case Expose:
	    EventuallyRedrawIcon(icon);
	    break;
	case DestroyNotify:
	    if (!(icon->flags & ICON_FLAG_DELETED)) {
		icon->flags |= ICON_FLAG_DELETED;
		Tcl_EventuallyFree((char*)icon, IconDestroy);
		Tk_DeleteGenericHandler(IconGenericHandler, cd);
	    } 
	    break;
	case ConfigureNotify:
	    icon->flags |=ICON_FLAG_RESHAPE_ON_REDRAW;
	    EventuallyRedrawIcon(icon);
	    break;
	case ReparentNotify:
	    if (ev->xreparent.parent == 
		    XRootWindowOfScreen(Tk_Screen(icon->tkwin))) {
		icon -> tray = None;
		icon -> flags &= (~ICON_FLAG_MANAGED);
		Tcl_DoWhenIdle((Tcl_IdleProc*)TryDock, (ClientData)icon);
	    } else {
		icon -> tray = ev->xreparent.parent;
	    }
	    break;
    }
}

static int PostBalloon(DockIcon* icon, const char * utf8msg, 
	long timeout)
{
    Tk_Window tkwin = icon -> tkwin;
    Display* dpy = Tk_Display(tkwin);
    if (icon->tray == None) 
	return 0;
    int length = strlen(utf8msg);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = icon->wrapper;
    ev.xclient.message_type = 
	Tk_InternAtom(tkwin,"_NET_SYSTEM_TRAY_OPCODE");
    ev.xclient.format = 32;
    ev.xclient.data.l[0]=time(NULL);
    ev.xclient.data.l[1]=SYSTEM_TRAY_BEGIN_MESSAGE;
    ev.xclient.data.l[2]=timeout;
    ev.xclient.data.l[3]=length;
    ev.xclient.data.l[4]=++icon->msgid;
    XSendEvent(dpy, icon->tray, False, NoEventMask, &ev);
    XSync(dpy, False);
    /* Sending message elements */
    ev.xclient.message_type =
	Tk_InternAtom(tkwin,"_NET_SYSTEM_TRAY_MESSAGE_DATA");
    ev.xclient.format = 8;
    while (length>0) {
	ev.type = ClientMessage;
	ev.xclient.window = icon->wrapper;
	ev.xclient.message_type = 
	    Tk_InternAtom(tkwin,"_NET_SYSTEM_TRAY_MESSAGE_DATA");
	ev.xclient.format = 8;
	memset(ev.xclient.data.b,0,20);
	strncpy(ev.xclient.data.b,utf8msg,20);
	XSendEvent(dpy, icon->tray, False, StructureNotifyMask, &ev);
	XSync(dpy, False);
	utf8msg+=20;
	length-=20;
    }
    return icon->msgid;
}

static int TrayIconCreateCmd(ClientData cd, Tcl_Interp *interp,
	int objc, Tcl_Obj * CONST objv[]) 
{
    DockIcon *icon;
    if (objc < 2||(objc%2)) {
	Tcl_WrongNumArgs(interp, 1, objv, "pathName ?option value ...?");
	return TCL_ERROR;
    }
    Tk_Window tkwin = 
	Tk_CreateWindowFromPath(interp, Tk_MainWindow(interp), 
		Tcl_GetString(objv[1]),"");
    if (tkwin == NULL) 
	return TCL_ERROR;
    TkpWmSetState((TkWindow*)tkwin, WithdrawnState);
    TKU_CompleteToplevel(tkwin);
    icon = (DockIcon*)Tcl_Alloc(sizeof(DockIcon));
    memset(icon,0,sizeof(DockIcon));
    icon->tkwin = tkwin;
    icon->root = RootWindowOfScreen(Tk_Screen(tkwin));
    icon->wrapper = TKU_Parent(Tk_Display(tkwin),Tk_WindowId(tkwin));
    icon->docksel = DockSelectionAtomFor(tkwin);
    icon->msgid = 1;

    icon->interp = interp;
    TKU_InitImageRep(&icon->img);
    icon->img.changed_proc = IconImageChanged;
    icon->img.client_data = (ClientData)icon;

    Tk_MakeWindowExist(icon->tkwin);

    icon->options = 
	Tk_CreateOptionTable(interp,IconOptionSpec);

    Tk_InitOptions(interp,(char*)icon,icon->options,icon->tkwin);

    Tk_SetOptions(interp,(char*)icon, icon->options, objc-2, objv+2, 
	    icon->tkwin, NULL, NULL);
    UpdateIcon(icon, ICON_CONF_IMAGE|ICON_CONF_XEMBED|
	    ICON_CONF_REDISPLAY|ICON_CONF_CLASS);

    icon->widgetCmd =
	Tcl_CreateObjCommand(interp, Tcl_GetString(objv[1]),
		TrayIconObjectCmd, (ClientData)icon, TrayIconDeleteProc);
    Tk_CreateEventHandler(tkwin,ExposureMask|StructureNotifyMask,
	    IconEvent,(ClientData)icon);
    TryDock(icon);
    return TCL_OK;
}



int Tktray_Init ( Tcl_Interp* interp ) 
{
    if (Tcl_InitStubs( interp, "8.4", 0) == NULL)
	return TCL_ERROR;
    if (Tk_InitStubs( interp, "8.4", 0) == NULL)
	return TCL_ERROR;
    Tcl_CreateObjCommand( interp, "::tktray::icon", 
	    TrayIconCreateCmd, NULL, NULL );
    Tcl_PkgProvide( interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}
