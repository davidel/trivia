#!/usr/bin/perl -w
#
# xfcrawl.pl by Davide Libenzi (fetch typed content from the net)
# Copyright (C) 2009  Davide Libenzi
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# Davide Libenzi <davidel@xmailserver.org>
#

use strict;
use Getopt::Long;
use LWP;
use HTTP::Cookies;
use Digest::SHA1 qw(sha1 sha1_hex);

my $stop = 0;
my $help = 0;
my $debug = 1;
my $sel_ua = "MOZWIN";
my $timeo = 30;
my $dtime = 30;
my $odir = ".";
my $fcount = -1;
my $gcount = 100;
my $base = 0;
my $doc_type;
my $max_size = 10 * 1024 * 1024;
my $cookies_file;
my @allow_types;
my %user_agents = (
    MOZWIN => 'Mozilla/5.0 (Windows; U; Windows NT 5.1; en-US; rv:1.7) Gecko/20040613 Firefox/0.8.0+',
    MOZLNX => 'Mozilla/5.0 (X11; U; Linux x86_64; en-US; rv:1.9.0.11) Gecko/2009061208 Iceweasel/3.0.12 (Debian-3.0.12-1)'
    );


Getopt::Long::Configure("bundling", "no_ignore_case");
GetOptions("debug|D=i"         => \$debug,
	   "user-agent|U=s"    => \$sel_ua,
	   "outdir|d=s"        => \$odir,
	   "count|c=i"         => \$fcount,
	   "doc-type|t=s"      => \$doc_type,
	   "base|b=i"          => \$base,
	   "timeout|T=i"       => \$timeo,
	   "max-size|M=i"      => \$max_size,
	   "delay|y=i"         => \$dtime,
	   "cookies-file|K=s"  => \$cookies_file,
	   "ctype|C=s"         => \@allow_types,
	   "help|h"            => \$help);

usage() if (!defined($doc_type) || $help);

fatal(2, "Output directory does not exist: ", $odir, "\n") if (! -d $odir);

$doc_type = lc($doc_type);

my $dbfile = "${odir}/xfcrawl-${doc_type}.db";

$SIG{'INT'} = 'INT_handler';

my $ua = LWP::UserAgent->new();
my $cjar;

if (defined($cookies_file)) {
    $cjar = HTTP::Cookies::Netscape->new(file => $cookies_file,
					 autosave => 1);
    $ua->cookie_jar($cjar);
}

$ua->timeout($timeo);
$ua->agent($user_agents{$sel_ua});

my %rdb;

load_db($dbfile, \%rdb);

for (; $fcount < 0 || $base < $fcount; $base += $gcount) {
    my $stime = time();

    dprint(5, "BASE: ", $base, "\tCOUNT: ", $gcount, "\n");

    my @uar;
    my $ucount = fetch_search($ua, $doc_type, $base, $gcount, \%rdb, \@uar);

    foreach my $url (@uar) {
	fetch_file($ua, $odir, $url, \%rdb);
	last if ($stop);
    }
    last if ($ucount == 0 || $stop);

# Do not stress Google too much. We play nice with them, so they won't
# blacklist our IPs.
    my $now = time();

    if ($now < $stime + $dtime) {
	dprint(5, "Pause for ", $dtime + $stime - $now, " second(s)\n");
	sleep($dtime + $stime - $now);
    }
}

dprint(1, "\n\nStopped!\n") if ($stop);

dprint(1, "Writing DB file ...\n");
save_db($dbfile, \%rdb);

dprint(1, "Done!\n");
exit(0);



sub INT_handler {
    $stop = 1;
}

sub fatal {
    my ($code, @msgs) = @_;

    foreach my $msg (@msgs) {
	print STDERR $msg;
    }
    exit($code);
}

sub dprint {
    my ($lev, @msgs) = @_;

    if ($debug >= $lev) {
	foreach my $msg (@msgs) {
	    print STDERR $msg;
	}
    }
}

sub usage {
    print STDERR "Use: $0 [-UdctbKTMh] -D DTYPE\n\n",
    "\t-D LEV        = Sets the debug level\n",
    "\t-U AGENT      = Sets the user agent\n",
    "\t-d DIR        = Sets the output directory\n",
    "\t-c COUNT      = Sets the number of results to be scanned\n",
    "\t-t DTYPE      = Sets the document type\n",
    "\t-b BASE       = Sets the base count for the results fetching\n",
    "\t-T TIMEOUT    = Sets the timeout in seconds\n",
    "\t-M SIZE       = Sets the maximum size of the files to be fetched\n",
    "\t-K FILE       = Sets the cookies file path\n",
    "\t-y DELAY      = Sets the delay time between two empty result sets\n",
    "\t-h            = Prints this help page\n\n";
    exit(1);
}

sub save_db {
    my ($fn, $rdb) = @_;

    fatal(3, "Unable to create DB file: ", $fn, "\n") if (!open(DFIL, ">$fn"));
    foreach my $url (keys %$rdb) {
	print DFIL $url, "\t", $$rdb{$url}, "\n";
    }
    close(DFIL);
}

sub load_db {
    my ($fn, $rdb) = @_;

    return 0 if (!open(DFIL, $fn));

    while (<DFIL>) {
	my $ln;

	($ln = $_) =~ s/^([^\r\n]*)[\r\n]*$/$1/g;

	my @argz = split(/[\t]+/, $ln);

	$$rdb{$argz[0]} = $argz[1];
    }
    close(DFIL);

    return 1;
}

sub fetch_search_raw {
    my ($ua, $dtype, $base, $cnt) = @_;

    my $url = "http://www.google.com/search?safe=off&num=${cnt}&q=filetype:${dtype}&start=${base}&sa=N&filter=0";

    my $req = HTTP::Request->new(GET => $url);

    return $ua->request($req);
}

sub extract_urls {
    my ($burl, $dtype, $data, $rdb, $uar) = @_;
    my $uhost;
    my $count = 0;
    my %ldb;

    ($uhost = $burl) =~ s/^([a-z]+:\/\/[^\/]+)/$1/i if (defined($burl));
    for (;;) {
	my $pos;

	last if (($pos = index($$data, "href=\"", -1)) < 5);

	$$data = substr($$data, $pos + 6);

	next if ($$data !~ /^([^"]+)\"/i);

	my $url = $1;

	if ($url !~ /^[a-z]+:\/\//i) {
	    next if (!defined($uhost));
	    $url = "/" . $url if ($url !~ /^\//);
	    $url = $uhost . $url;
	}

	next if ($url !~ /(http|ftp|https):\/\//i);

	$url = $1 if ($url =~ /^([^\&\?]+)/);

	$url = lc($url);
	if ($url =~ /\.${dtype}$/i && !defined($$rdb{$url}) && !defined($ldb{$url})) {
	    dprint(4, "Parsing: ", $url, "\n");
	    $ldb{$url} = 1;
	    push(@$uar, $url);
	}
	$count++;
    }

    return $count;
}

sub fetch_search {
    my ($ua, $dtype, $base, $cnt, $rdb, $uar) = @_;

    my $res = fetch_search_raw($ua, $dtype, $base, $cnt);

    fatal(3, "Search HTTP error: ", $res->status_line, "\n") if (!$res->is_success);

    dprint(10, "Search Data:\n",
	   "--------------------------------------------------------------\n",
	   $res->content,
	   "\n--------------------------------------------------------------\n");

    return extract_urls(undef, $dtype, \$res->content, $rdb, $uar);
}

sub get_http_res {
    my ($ua, $method, $url) = @_;

    my $req = HTTP::Request->new($method => $url);

    my $res = $ua->request($req);

    dprint(1, "URL: ", $url, "\n",
	   "\tHTTP error: ", $res->status_line, "\n") if (!$res->is_success);

    return $res;
}

sub allowed_ctype {
    my ($ctype) = @_;

    foreach my $ct (@allow_types) {
	return 1 if ($ctype =~ /$ct/i);
    }

    return ($ctype =~ /text\//i) ? 0: 1;
}

sub fetch_file {
    my ($ua, $dir, $url, $rdb) = @_;

    dprint(5, "Peeking: ", $url, "\n");

    my $res = get_http_res($ua, "HEAD", $url);

    if (!$res->is_success) {
	$$rdb{$url} = "ERROR=" . $res->code;
	return 0;
    }

    my $size = $res->header("Content-Length") || -1;
    my $ctype = lc($res->content_type);

    dprint(5, "CTYPE: ", $ctype, "\n",
	   "SIZE: ", $size, "\n");
    if (!allowed_ctype($ctype)) {
	$$rdb{$url} = "CTYPE=" . $ctype;
	dprint(5, "\tDropped (bad type)\n");
	return 0;
    }
    if ($max_size > 0 && $size > $max_size) {
	$$rdb{$url} = "SIZE=" . $size;
	dprint(5, "\tDropped (too big)\n");
	return 0;
    }

    dprint(3, "Fetching: ", $url, "\n");

    $res = get_http_res($ua, "GET", $url);

    if (!$res->is_success) {
	$$rdb{$url} = "ERROR=" . $res->code;
	return 0;
    }

    my $hsha = sha1_hex($res->content);
    my $sdir = substr($hsha, 0, 3);

    if (! -d "${dir}/${sdir}") {
	if (!mkdir("${dir}/${sdir}", 0777)) {
	    dprint(0, "Unable to create directory: ${dir}/${sdir}\n");
	    return -1;
	}
    }
    if (!open(CFIL, ">${dir}/${sdir}/${hsha}")) {
	dprint(0, "Unable to create file: ${dir}/${sdir}/${hsha}\n");
	return -1;
    }
    print CFIL $res->content;
    close(CFIL);

    $$rdb{$url} = $hsha;

    return 1;
}

