package lib

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
)

var PRE = flag.String("cmoc_pre", "", "prefix these flags to cmoc")

type RunSpec struct {
	AsmListingPath string
	LwAsm          string
	LwLink         string
	Cmoc           string
	OutputBinary   string
	Args           []string
	BorgesDir      string
}

func (rs RunSpec) RunCompiler(filename string) {
	args := []string{"--os9", "-S"}
	for _, a := range strings.Split(*PRE, " ") {
		if a != "" {
			args = append(args, a)
		}
	}
	args = append(args, filename)
	cmd := exec.Command(rs.Cmoc, args...)
	cmd.Stderr = os.Stderr
	log.Printf("RUNNING: %v", cmd)
	err := cmd.Run()
	if err != nil {
		log.Fatalf("cmoc compiler failed: %v: %v", cmd, err)
	}
}
func (rs RunSpec) TweakAssembler(filename string, directs map[string]bool) {
	err := os.Rename(filename+".s", filename+".s-orig")
	if err != nil {
		log.Fatalf("cannot rename %q to %q: %v",
			filename+".s", filename+".s-orig", err)
	}

	orig_filename := filename + ".s-orig"
	r, err := os.Open(orig_filename)
	if err != nil {
		log.Fatalf("cannot open: %q: %v", orig_filename, err)
	}
	new_filename := filename + ".s"
	w, err := os.Create(new_filename)
	if err != nil {
		log.Fatalf("cannot create: %q: %v", new_filename, err)
	}

	skip := 0
	scanner := bufio.NewScanner(r)
	for scanner.Scan() {
		s := scanner.Text()
		m1 := FindCommaYTab(s)
		m2 := FindCommaYEnd(s)
		if m1 != nil {
			s = fmt.Sprintf("%s\t%s", m1[1], m1[2])
		} else if m2 != nil {
			s = fmt.Sprintf("%s\t;", m2[1])
		}

		m3 := FindLeaxVar(s)
		if m3 != nil {
			s = fmt.Sprintf("%s\tLDX\t#%s\t%s", m3[1], m3[2], m3[3])
		}

		m4 := FindLbsrStkcheck(s)
		if m4 != nil {
			skip = 2
		}

		m5 := FindPotentiallyDirect(s)
		if m5 != nil {
			if _, ok := directs[m5[4]]; ok {
				s = fmt.Sprintf("%s\t%s\t%s<%s%s", m5[1], m5[2], m5[3], m5[4], m5[5])
			}
		}

		if skip == 0 {
			fmt.Fprintf(w, "%s\n", s)
		} else {
			skip--
		}
	}
	if err := scanner.Err(); err != nil {
		log.Fatalf("reading standard input:", err)
	}
	r.Close()
	w.Close()
}

var FindCommaYTab = regexp.MustCompile("(.*),Y\t(.*)").FindStringSubmatch
var FindCommaYEnd = regexp.MustCompile("(.*),Y$").FindStringSubmatch
var FindLeaxVar = regexp.MustCompile("(.*)\tLEAX\t([[:word:]]+[+][[:digit:]]+)\t(.*)").FindStringSubmatch
var FindLbsrStkcheck = regexp.MustCompile("LBSR\t_stkcheck").FindStringSubmatch
var FindPotentiallyDirect = regexp.MustCompile("(.*)\t(LDD|STD|ADDD|CMPD|LDX|STX|LEAX)\t([[]?)(_[[:word:]]+)([+]0.*)").FindStringSubmatch

func (rs RunSpec) RunAssembler(filename string) {
	cmd := exec.Command(
		rs.LwAsm, "--obj", "--6809",
		"--list="+filename+".o.list",
		"-o", filename+".o",
		filename+".s")
	log.Printf("RUNNING: %v", cmd)
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	err := cmd.Run()
	if err != nil {
		log.Fatalf("lwasm assembler failed: %v: %v", cmd, err)
	}
}
func (rs RunSpec) RunLinker(ofiles []string, outbin string) {
	cmdargs := []string{"--os9", "-i", "--lwlink=" + rs.LwLink}
	cmdargs = append(cmdargs, ofiles...)
	cmd := exec.Command(rs.Cmoc, cmdargs...)
	log.Printf("RUNNING: %v", cmd)
	err := cmd.Run()
	if err != nil {
		log.Fatalf("cmoc/lwlink linker failed: %v: %v", cmd, err)
	}
}
func (rs RunSpec) RunAll() {
	if len(rs.Args) == 0 {
		log.Fatalf("no filanames to compile")
	}

	directs := make(map[string]bool)
	alists := make(map[string]map[string][]*AsmListingRecord)
	var lmap []*LinkerMapRecord
	for phase := 1; phase <= 2; phase++ {
		var ofiles []string
		for _, filename := range rs.Args {
			if !strings.HasSuffix(filename, ".c") {
				log.Fatalf("filename should end in .c: %q", filename)
			}
			rs.RunCompiler(filename)
			filebase := strings.TrimSuffix(filename, ".c")
			rs.TweakAssembler(filebase, directs)
			rs.RunAssembler(filebase)
			if phase == 2 {
				alist := ReadAsmListing(filebase + ".o.list")
				alists[Basename(filename)] = alist
			}
			ofiles = append(ofiles, filebase+".o")
		}
		rs.RunLinker(ofiles, rs.OutputBinary)

		// Read the linker map.
		// The first time is for the `directs` set of potential direct page variables.
		// The second time is for finding all the linked object files and output the final listing.
		lmap = ReadLinkerMap(rs.OutputBinary + ".map")
		for _, e := range lmap {
			if e.Section == "" && strings.HasPrefix(e.Symbol, "_") && e.Start < 256 {
				directs[e.Symbol] = true
			}
		}
	}

	listing_dirs := strings.Split(rs.AsmListingPath, ":")
	SearchForNeededListings(alists, lmap, listing_dirs)

	//for filename, alist := range alists {
	//for section, records := range alist {
	//for _, rec := range records {
	//log.Printf("%q ... %q ... %#v", filename, section, *rec)
	//}
	//}
	//}

	mod, err := ioutil.ReadFile(rs.OutputBinary)
	if err != nil {
		log.Fatalf("Cannot read Output Binary: %q: %v", rs.OutputBinary, err)
	}

	modname := GetOs9ModuleName(mod)
	log.Printf("Module Name: %q", modname)
	log.Printf("Module Length: %04x", len(mod))
	checksum := mod[len(mod)-3:]
	log.Printf("Module CheckSum: %02x", checksum)
	borges_version := fmt.Sprintf("%s.%04x%02x", strings.ToLower(modname), len(mod), checksum)
	log.Printf("borges Version: %q", borges_version)

	list_out_filename := rs.OutputBinary + ".listing"
	if rs.BorgesDir != "" {
		// Change to use the lowercase module name and version suffix, in the Borges Dir.
		list_out_filename = filepath.Join(rs.BorgesDir, borges_version)
	}

	fd, err := os.Create(list_out_filename)
	if err != nil {
		log.Fatalf("Cannot create Output listing: %q: %v", list_out_filename, err)
	}
	w := bufio.NewWriter(fd)

	OutputFinalListing(lmap, alists, mod, w)
	w.Flush()
	fd.Close()

	log.Printf("WROTE FINAL LISTING TO %q", list_out_filename)
}

func OutputFinalListing(
	lmap []*LinkerMapRecord,
	alists map[string]map[string][]*AsmListingRecord,
	mod []byte,
	w io.Writer) {
	for _, rec := range lmap {
		if rec.Section == "" {
			// It's a Symbol, not a Section.
			continue
		}
		if rec.Section == "bss" {
			// BSS have no instructions.
			continue
		}
		start := rec.Start
		n := rec.Length
		if n == 0 {
			continue
		}
		f := Basename(rec.Filename)
		alist, ok := alists[f]
		if !ok {
			log.Printf("Missing alist file: %q -> %q", rec.Filename, f)
			continue
		}
		seclist, ok := alist[rec.Section]
		if !ok {
			log.Printf("Missing alist section: %q -> %q; %q: %#v", rec.Filename, f, rec.Section, rec)
			continue
		}
		fmt.Fprintf(w, "\n")
		for _, line := range seclist {
			if line.Location >= n {
				log.Printf("Line location too big (>= %d): %#v", n, line)
				continue
			}

			hex := line.Bytes
			/*
			   if len(hex) > 16 {
			       hex = hex[:16]
			   }
			*/
			name := line.Filename
			/*
			   if len(name) > 17 {
			       name = name[:17]
			   }
			*/

			inst := fmt.Sprintf("%s:%05d | %s", strings.Trim(name, " "), line.LineNum, line.Instruction)
			fmt.Fprintf(w, "%04X %-16s (%s):%05d         %s\n", line.Location+start, hex, name, line.LineNum, inst)
		}
		fmt.Fprintf(w, "\n")
	}
}

func GetOs9ModuleName(mod []byte) string {
	if mod[0] != 0x87 || mod[1] != 0xCD {
		panic("bad header")
	}
	expectedLen := int(mod[2])*256 + int(mod[3])
	if len(mod) != expectedLen {
		panic("bad length")
	}
	i := int(mod[4])*256 + int(mod[5])
	var z []byte
	for ; 0 == (mod[i] & 0x80); i++ {
		z = append(z, mod[i])
	}
	z = append(z, mod[i]&0x7F)
	return string(z)
}

func Basename(s string) string {
	// base name (directory removed)
	base := filepath.Base(s)
	// only what is before the first '.'
	return strings.Split(base, ".")[0]
}

func UseBasenames(
	alists map[string]map[string][]*AsmListingRecord) {
	// save keys
	var keys []string
	for k := range alists {
		keys = append(keys, k)
	}
	// now mutate map
	for _, key := range keys {
		alists[Basename(key)] = alists[key]
	}
}

func SearchForNeededListings(
	alists map[string]map[string][]*AsmListingRecord,
	lmap []*LinkerMapRecord,
	dirs []string) {
	// Use basenames in alists.
	UseBasenames(alists)

	for _, base := range BasenamesOfLinkerMap(lmap) {
		log.Printf("LINKER NAME %q", base)
		for _, dir := range dirs {
			filename := filepath.Join(dir, base+".os9_o.list")
			println("CHECK", filename)
			fd, err := os.Open(filename)
			println(fd, err)
			if err == nil {
				alist := ReadAsmListing(filename)
				for section, records := range alist {
					for _, rec := range records {
						log.Printf("%q... %q ... %#v", filename, section, *rec)
					}
				}
				alists[base] = alist
				fd.Close()
			}
		}
	}

}