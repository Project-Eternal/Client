# Stand-in for the part of IPC::Cmd that OpenSSL's Configure uses (can_run), for machines without the perl-IPC-Cmd package.
package IPC::Cmd;
use strict;
use File::Spec;
use Exporter 'import';
our @EXPORT_OK = qw(can_run);
sub can_run {
    my ($name) = @_;
    return $name if File::Spec->file_name_is_absolute($name) && -x $name;
    for my $dir (File::Spec->path()) {
        my $p = File::Spec->catfile($dir, $name);
        return $p if -x $p && !-d $p;
    }
    return undef;
}
1;
