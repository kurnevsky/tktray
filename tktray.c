#include <tcl.h>
#include <tk.h>
#include <tkInt.h>
#include <tkIntPlatDecls.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

#include <X11/X.h>
#include <X11/Xutil.h>
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
#define ICON_CONF_IMAGE (1<<0)	    /* Image changed */
#define ICON_CONF_REDISPLAY (1<<1)  /* Redisplay required */
#define ICON_CONF_XEMBED (1<<2)	    /* Remapping or unmapping required */
#define ICON_CONF_CLASS (1<<3)	    /* TODO WM_CLASS update required */

/* Widget states */
#define ICON_FLAG_REDRAW_PENDING (1<<0)
#define ICON_FLAG_RESHAPE (1<<1)

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
    Tk_Window wrapper = (Tk_Window)TkpGetWrapperWindow((TkWindow*)w);
    if (!wrapper) {
	Tk_MakeWindowExist(w);
	TkpWmSetState((TkWindow*)w,WithdrawnState);
	Tk_MapWindow(w);
	wrapper = (Tk_Window)TkpGetWrapperWindow((TkWindow*)w);
    }
    return wrapper;
}

static Window TKU_XID(Tk_Window w)
{
    Window xid = Tk_WindowId(w);
    if (xid == None) {
	Tk_MakeWindowExist(w);
	xid = Tk_WindowId(w);
    }
    return xid;
}



/* Data structure representing dock widget */
typedef struct {
    /* standard for widget */
    Tk_Window tkwin, drawingWin;
    Window wrapper;
    Window root;
    Window manager;

    Tk_OptionTable options;
    Tcl_Interp *interp;
    Tcl_Command widgetCmd;

    Tk_Image image; /* image to be drawn */

    Atom aMANAGER;
    Atom a_NET_SYSTEM_TRAY_Sn;
    Atom a_XEMBED_INFO;
    Atom a_NET_SYSTEM_TRAY_MESSAGE_DATA;
    Atom a_NET_SYSTEM_TRAY_OPCODE;

    int flags; /* ICON_FLAG_ - see defines above */
    int msgid; /* Last balloon message ID */
    int useShapeExt;
    
    int x,y,width, height;
    int visible; /* whether XEMBED_MAPPED should be set */
    char *imageString, /* option: -image as string */
	*classString; /* option: -class as string */
} DockIcon;

static void EventuallyRedrawIcon(DockIcon* icon);
static void DisplayIcon(ClientData cd);
static int IconGenericHandler(ClientData cd, XEvent *ev);

static int PostBalloon(DockIcon* icon, const char * utf8msg, 
		       long timeout);
static int TrayIconCreateCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[]);
static int TrayIconObjectCmd(ClientData cd, Tcl_Interp *interp,
			     int objc, Tcl_Obj * CONST objv[]);
static void TrayIconDeleteProc( ClientData cd );
static Atom DockSelectionAtomFor(Tk_Window tkwin);
static void Dock(DockIcon *icon, Window manager);

static int TrayIconConfigureMethod(DockIcon *icon, Tcl_Interp* interp,
				   int objc, Tcl_Obj* CONST objv[], 
				   int addflags);

static void TrayIconUpdate(DockIcon* icon, int mask);
static void RetargetEvent(DockIcon *icon, XEvent *ev);
static void TrayIconUpdate(DockIcon* icon, int mask);
static int TrayIconConfigureMethod(DockIcon *icon, Tcl_Interp* interp,
				   int objc, Tcl_Obj* CONST objv[], 
				   int addflags);
    

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

    enum {XWC_CONFIGURE=0, XWC_BALLOON, XWC_BBOX};
    const char *st_wcmd[]={"configure","balloon","bbox",NULL};
    long timeout = 0;

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
    case XWC_BALLOON:
	/* FIXME: not implemented yet */
	if ((objc!=3) && (objc!=4)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "message ?timeout?");
	    return TCL_ERROR;
	}
	if (objc==4) {
	    if (Tcl_GetLongFromObj(interp,objv[3],&timeout)!=TCL_OK)
		return TCL_ERROR;
	}
	PostBalloon(icon,Tcl_GetString(objv[2]), timeout);
	return TCL_OK;
    case XWC_BBOX:
	if (icon->drawingWin) {
	    XGetWindowAttributes(Tk_Display(icon->drawingWin),
				 TKU_XID(icon->drawingWin),
				 &xwa);

	    XTranslateCoordinates(Tk_Display(icon->drawingWin),
				  TKU_XID(icon->drawingWin),
				  xwa.root, 0,0,&icon->x,&icon->y, &bogus);
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
    }
    return TCL_OK;
}

static Atom DockSelectionAtomFor(Tk_Window tkwin) 
{
    char buf[256];
    snprintf(buf,sizeof(buf),"_NET_SYSTEM_TRAY_S%d",Tk_ScreenNumber(tkwin));
    return Tk_InternAtom(tkwin,buf);
}


static void Dock(DockIcon *icon, Window manager) 
{
    Tk_Window tkwin = icon->drawingWin;
    Display *dpy = Tk_Display(tkwin);
    Tk_Window wrapper;
    XEvent ev;
    int w=24,h=24;
    long info[] = { 0, XEMBED_MAPPED };

    Tk_SetWindowBackgroundPixmap(tkwin, ParentRelative);
    wrapper = TKU_Wrapper(tkwin);
    if (icon->image) {
	Tk_SizeOfImage(icon->image,&w,&h);
    }
    Tk_SetMinimumRequestSize(icon->drawingWin,w,h);
    Tk_GeometryRequest(icon->drawingWin,w,h);
    Tk_SetGrid(icon->drawingWin,1,1,w,h);
    Tk_SetWindowBackgroundPixmap(wrapper, ParentRelative);
    icon->wrapper = TKU_XID(wrapper);

    XChangeProperty(Tk_Display(tkwin), 
		    TKU_XID(wrapper), 
		    icon->a_XEMBED_INFO, icon->a_XEMBED_INFO, 32, 
		    PropModeReplace, (unsigned char*)info, 2);

    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = icon->manager = manager;
    ev.xclient.message_type = icon->a_NET_SYSTEM_TRAY_OPCODE;
    ev.xclient.format = 32;
    ev.xclient.data.l[0]=0;
    ev.xclient.data.l[1]=SYSTEM_TRAY_REQUEST_DOCK;
    ev.xclient.data.l[2]=icon->wrapper;
    ev.xclient.data.l[3]=0;
    ev.xclient.data.l[4]=0;
    XSendEvent(dpy, manager, True, NoEventMask, &ev);
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
    {TK_OPTION_BOOLEAN,"-shape","shape","Shape",
     "0", -1, Tk_Offset(DockIcon, useShapeExt), 
     0, (ClientData) NULL,
     ICON_CONF_IMAGE | ICON_CONF_REDISPLAY},
    {TK_OPTION_BOOLEAN,"-visible","visible","Visible",
     "1", -1, Tk_Offset(DockIcon, visible), 
     0, (ClientData) NULL,
     ICON_CONF_XEMBED},
    {TK_OPTION_END}
};


static void
TrayIconImageChanged(ClientData cd, int x, int y, int w, int h, int imgw, int imgh)
{
    DockIcon *icon = (DockIcon*) cd;
    if (icon->drawingWin)
	Tk_GeometryRequest(icon->drawingWin, imgw, imgh);
    EventuallyRedrawIcon(icon);
    icon->flags |= ICON_FLAG_RESHAPE;
}


static 
void EventuallyRedrawIcon(DockIcon* icon) {
    if (!(icon->flags&ICON_FLAG_REDRAW_PENDING)) {
	icon->flags|=ICON_FLAG_REDRAW_PENDING;
	Tcl_DoWhenIdle(DisplayIcon,(ClientData)icon);
    }
}


static void DisplayIcon(ClientData cd)
{
    DockIcon *icon = (DockIcon*)cd;
    const Tk_ImageType *imageType;
    int w,h;

    icon->flags&=(~ICON_FLAG_REDRAW_PENDING);

    if (icon->image) {
	Tk_SizeOfImage(icon->image,&w,&h);
    }

    if (icon->drawingWin) {
	if (icon->useShapeExt && 
	    (icon->flags & ICON_FLAG_RESHAPE))
	{ 
	    if (icon->imageString && icon->image) {
		Tk_PhotoHandle phh =
		    (Tk_PhotoHandle)Tk_GetImageMasterData(icon->interp,
							  icon->imageString,
							  &imageType);
		if (imageType &&
		    !strcmp(imageType->name,"photo")) {
		    XShapeCombineRegion(Tk_Display(icon->drawingWin),
					TKU_XID(TKU_Wrapper(icon->drawingWin)),
					ShapeBounding,
					(icon->width - w)/2,
					(icon->height - h)/2,
					(Region)
					TkPhotoGetValidRegion(phh),
					ShapeSet);
		}
	    }
	}
	icon->flags &= ~ICON_FLAG_RESHAPE;
	XClearWindow(Tk_Display(icon->drawingWin),
		     TKU_XID(icon->drawingWin));
	if (icon->image) {
	    Tk_RedrawImage(icon->image,0,0,w,h,
			   TKU_XID(icon->drawingWin),
			   (icon->width - w)/2,
			   (icon->height - h)/2);
	}
    }
}

static void RetargetEvent(DockIcon *icon, XEvent *ev)
{
    int send = 0;
    Window* saveWin1 = NULL, *saveWin2 = NULL;
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

static void TrayIconEvent(ClientData cd, XEvent* ev)
{
    DockIcon *icon = (DockIcon*)cd;
    int nx,ny,nw,nh;
    Window bogus;

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
	Tcl_CancelIdleCall(DisplayIcon,(ClientData)icon);
	icon->flags &= ~ICON_FLAG_REDRAW_PENDING;
	icon->drawingWin = NULL;
	icon->wrapper = None;
	TrayIconUpdate(icon,ICON_CONF_XEMBED); /* find other dock or wait */
	break;
    case ConfigureNotify:
	icon->width = ev->xconfigure.width;
	icon->height = ev->xconfigure.height;
	TKU_XID(TKU_Wrapper(icon->drawingWin));
	TKU_XID(TKU_Wrapper(icon->tkwin));
	Tk_GetRootCoords(icon->drawingWin,&nx,&ny);
	Tk_GeometryRequest(icon->tkwin,icon->width,icon->height);
	Tk_MoveToplevelWindow(icon->tkwin,nx,ny);
	icon->x = nx;
	icon->y = ny;
	icon->flags |= ICON_FLAG_RESHAPE;
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
	if(icon->drawingWin) {
	    icon->visible = 0;
	    Tcl_CancelIdleCall(DisplayIcon,(ClientData)icon);
	    icon->flags &= ~ICON_FLAG_REDRAW_PENDING;
	    Tk_DestroyWindow(icon->drawingWin);
	}
	if(icon->image) 
	    Tk_FreeImage(icon->image);
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
    
    if (!(icon->drawingWin)) 
	return 0;

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
    
    XSendEvent(dpy, icon->manager , True, NoEventMask, &ev);
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
	XSendEvent(dpy, icon->manager, True, StructureNotifyMask, &ev);
	XSync(dpy,False);
	utf8msg+=20;
	length-=20;
    }
    return icon->msgid;
}

/* For non-tk events */
static int IconGenericHandler(ClientData cd, XEvent *ev)
{
    DockIcon *icon = (DockIcon*)cd;

    if ((ev->type == ClientMessage) &&
	(ev->xclient.message_type == icon->aMANAGER) &&
	((Atom)ev->xclient.data.l[1] == icon->a_NET_SYSTEM_TRAY_Sn)) {
	TrayIconUpdate(icon, ICON_CONF_XEMBED);
	return 1;
    }
    return 0;
}


/* Get in touch with new options that are certainly valid */
static void TrayIconUpdate(DockIcon* icon, int mask)
{
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
	if (!icon->visible) {
	    if (!icon->drawingWin) {
		/* probably we're running generic handler */
		Tk_DeleteGenericHandler(IconGenericHandler, (ClientData)icon);
	    } else {
		/* Xembedding ends right here,
		   and no generic handler is running */
		Tk_DestroyWindow(icon->drawingWin);
		icon->drawingWin = NULL; 
		icon->wrapper = None;
	    }
	} else { /* icon should be visible but isn't (and we know no window present) */
	    Window manager = XGetSelectionOwner(Tk_Display(icon->tkwin),icon->a_NET_SYSTEM_TRAY_Sn);
	    if (manager != None) {
		if (!icon->drawingWin) {
		    Tcl_SavedResult oldResult;
		    Tcl_SaveResult(icon->interp, &oldResult);
		    /* icon->drawingWin = Tk_CreateAnonymousWindow(icon->interp, icon->tkwin, NULL); */
		    icon->drawingWin = Tk_CreateWindow(icon->interp, icon->tkwin, "inner", "");
		    Tk_SetClass(icon->drawingWin,"TkTrayInnerPart");
		    if (icon->drawingWin) {
			Tk_CreateEventHandler(icon->drawingWin,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|
					      EnterWindowMask|LeaveWindowMask|PointerMotionMask,
					      TrayIconEvent,(ClientData)icon);
			Dock(icon, manager);
		    } else {
			Tcl_BackgroundError(icon->interp);
		    }
		    Tcl_RestoreResult(icon->interp, &oldResult);
		} 
	    } else {
		Tk_CreateGenericHandler(IconGenericHandler, (ClientData)icon);
	    }
	}
    }
    if (mask & ICON_CONF_IMAGE) {
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

    if (objc<=1) {
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
	if (newImage) {
	    int w,h;
	    Tk_SizeOfImage(newImage,&w,&h);
	    if(icon->drawingWin) {
		Tk_GeometryRequest(icon->drawingWin,w,h);
		Tk_SetMinimumRequestSize(icon->drawingWin,w,h);
	    }
	}
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

    icon->root = RootWindowOfScreen(Tk_Screen(icon->tkwin));
    icon->a_NET_SYSTEM_TRAY_Sn = DockSelectionAtomFor(icon->tkwin);
    icon->a_NET_SYSTEM_TRAY_OPCODE = Tk_InternAtom(icon->tkwin,"_NET_SYSTEM_TRAY_OPCODE");
    icon->a_XEMBED_INFO = Tk_InternAtom(icon->tkwin,"_XEMBED_INFO");
    icon->aMANAGER = Tk_InternAtom(icon->tkwin,"MANAGER");
    icon->interp = interp;


    if (objc>3) {
	if (TrayIconConfigureMethod(icon, interp, objc-2, objv+2, ICON_CONF_XEMBED|ICON_CONF_IMAGE)!=TCL_OK) {
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
    Tcl_Eval(interp,"bind TkTrayInnerPart <Configure> {event generate [winfo parent %W] <<IconConfigure>>}");
    Tcl_Eval(interp,"bind TkTrayInnerPart <Destroy> {event generate [winfo parent %W] <<IconDestroy>>}");
    Tcl_Eval(interp,"bind TkTrayInnerPart <Map> {event generate [winfo parent %W] <<IconCreate>>}");
    Tcl_PkgProvide( interp, PACKAGE_NAME, PACKAGE_VERSION);
    return TCL_OK;
}
