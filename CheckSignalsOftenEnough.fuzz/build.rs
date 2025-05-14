fn main() {
    cc::Build::new()
        .std("c11")
        .extra_warnings(true)
        .file("src/tdal.c")
        .compile("tdal");
}
