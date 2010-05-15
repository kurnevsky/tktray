#catch {load [lindex [glob ./libtktray1.*.so] end]} msg

package require tktray 1.0
wm withdraw .
set scriptdir [file normalize [file join [pwd] [file dirname [info script]]]]

if {[catch {
    catch { package require tkpng}
    catch { package require Img::png}
    image create photo ico1 -file [file join $scriptdir mixer.png]
#    image create photo ico2 -file [file join $scriptdir mixer-off.png]
    #image create bitmap ico2 -file [file join $scriptdir leftptr.xbm] \
	-maskfile [file join $scriptdir leftpmsk.xbm]
    image create photo ico2 -file [file join $scriptdir coccinella.png]
    image create photo ico3 -file [file join $scriptdir grad.png]
    image create photo ico4 -file [file join $scriptdir grad.png]
    set count 4
}]} {
    image create photo ico1 -file [file join $scriptdir floppy.gif]
    image create photo ico2 -file [file join $scriptdir floppy2.gif]
    set count 2
}

bind WildIcon <Create> {puts "created icon as %W"}

if {$count==4} {
set rrc 0
proc regrad {} {
    global rrc
    after 200 regrad
    ico4 copy ico3 -from 0 0 24 12 -compositingrule [if {[set rrc [expr {($rrc+1)%10}]]} {lindex overlay} {lindex set}]
}
regrad
}
set idx 1
if 1 {
    ::tktray::icon .myi -image ico2  -class WildIcon
    puts [winfo id .myi]
    bind .myi <1> {
	.myi configure -image ico$idx
	incr idx
	if {$idx > $count} { set idx 1 }
    }
    bind .myi <Motion> {puts "Over %X,%Y; Bbox [.myi bbox]; InnerGeo [wm geometry .myi.myi]"}
    bind .myi <Destroy> {puts "Hasta la vista"}
    bind .myi <Enter> {puts Now-In}
    bind .myi <Leave> {puts Now-Out}
    bind all <Leave> {puts "Out of %W"}
    bind .myi <Map> {puts Now-Is-Mapped}
    bind .myi <Unmap> {puts Now-Is-Unmapped}
    bind .myi <Configure> {puts "ORLY am I really in bbox [.myi bbox]"}
    bind .myi <<IconConfigure>> {puts "ORLY am I really in bbox [.myi bbox]"}
    bind .myi <<IconDestroy>> {puts "Illbe backson"}
    bind .myi <<IconCreate>> {puts "Im here"}
    bind . <F2> {.myi configure -visible 0}
    bind . <F3> {.myi configure -visible 1}
    bind . <F4> { .myi balloon "Hello, World! Howdy?" }
    bind .myi <3> {after 2000 {puts "2 secs elapsed, let's talk...";.myi balloon "Натурально, Бендер, вы не понимаете...\nМолчите! моя специальность - гусь!" } }
    bind .myi <2> { .myi configure -visible 0; after 5000 {.myi configure -visible 1}}
    bind .myi <Control-2> { .myi configure -docked 0; after 5000 {.myi configure -docked 1}}
    bind .myi <Shift-2> { .myi configure -image ""; after 5000 {.myi configure -image ico1}}
    bind .myi <Shift-3> { image delete ico1 }
    after 4000 {.myi balloon "Hello, World! Howdy?" }
}
