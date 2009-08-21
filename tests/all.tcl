#load ./libtktray1.0.so
package require tktray 1.0
set scriptdir [file normalize [file join [pwd] [file dirname [info script]]]]

image create photo ico -file [file join $scriptdir floppy.gif]
image create photo ico2 -file [file join $scriptdir floppy2.gif]

set idx 1
if 1 {
    ::tktray::icon .myi -image ico2
    bind .myi <1> {.myi configure -image [lindex {ico ico2} [expr {[incr idx]%%2}]]}
    bind .myi <Motion> {puts "Over %X,%Y; [winfo rootx .myi]; [winfo width .myi]"}
    bind .myi <Destroy> {puts "Hasta la vista"}
    bind .myi <Enter> {puts Now-In}
    bind .myi <Leave> {puts Now-Out}
    bind .myi <Map> {puts Now-Is-Mapped}
    bind .myi <Configure> {puts Configure:%w,%h}
    bind . <F2> {.myi configure -visible 0}
    bind . <F3> {.myi configure -visible 1}
    bind . <F4> {.myi balloon "Hello, World! Howdy?" }
}
