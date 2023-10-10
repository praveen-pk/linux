
import re
import subprocess
import json
import tarfile
import hashlib
import os

from lsgtools import log

# TODO check dependencies at startup
# - make


def get_tarball_format():
    # This probably should come from some config, but keep hardcoded for now
    return "tar"


def build_tarball_prefix(version):
    return "kernel-mshv-{}".format(version)


def build_tar_name(version):
    tarball_format = "tar"
    return "{}.{}".format(build_tarball_prefix(version), tarball_format)


def build_tgz_name(version):
    tarball_name = "{}.gz".format(build_tar_name(version))
    return tarball_name


def get_version_local():
    # TODO try/except
    res = subprocess.run(["make", "kernelversion"], stdout=subprocess.PIPE)
    return res.stdout.decode("ascii").strip()


def get_version_from_tag(ref):
    parts = ref.split("/")
    return parts[len(parts)-1]


def is_version_valid(version):
    ver_ptr = re.compile(r"\d+\.\d+\.\d+\.mshv\d+$")
    return ver_ptr.match(version) is not None


def patch_config(tarball_local_path, kernel_conf, conf_patch, kernel_basename):
    for k in kernel_conf:
        cf = kernel_conf[k]["path"]

        # Extract configs from the tarball into the SPEC
        config_path_in_tar = "arch/{}/configs/{}".format(kernel_conf[k]["kernel_arch"], kernel_basename)
        conf = None
        with tarfile.open(tarball_local_path, "r") as tar:
            # Expect just one root member in the tarball which is a dir
            for ti in tar:
                if ti.isdir():
                    config_path_in_tar = "{}/{}".format(ti.name, config_path_in_tar)
                    log.info("Reading '{}' from '{}'".format(config_path_in_tar, tarball_local_path))
                    conf = tar.extractfile(config_path_in_tar).read().decode("utf-8")
                    break
        tar.close()
        if not bool(conf):
            raise Exception("Read empty config from '{}' in '{}".format(config_path_in_tar, tarball_local_path))

        for sr in conf_patch:
            conf = re.sub(re.compile(sr[0]), sr[1], conf)

        with open(cf, "w") as f:
            f.write(conf)
        f.close()

        kernel_conf[k]["sha256"] = hashlib.sha256(conf.encode()).hexdigest()

    return kernel_conf


def update_spec_sums(kernel_sums_path, sums_struct):
    with open(kernel_sums_path, "w") as f:
        json.dump(sums_struct, f, indent=2)
    f.close()


def update_spec(kernel_spec_path, version, rev_hash):
    with open(kernel_spec_path, "r") as f:
        kernel_spec = f.read()
    f.close()
    kernel_spec = re.sub(re.compile("%global pkgrel.*"), "%global pkgrel 1000", kernel_spec)
    kernel_spec = re.sub(re.compile("%global kernelver.*"), "%global kernelver {}".format(version), kernel_spec)
    kernel_spec = re.sub(re.compile("%global gitrev.*"), "%global gitrev {}".format(rev_hash), kernel_spec)
    with open(kernel_spec_path, "w") as f:
        f.write(kernel_spec)
    f.close()


def update_kernel_version(src_tree, ver):
    mf = os.path.join(src_tree, "Makefile")
    with open(mf, "r") as f:
        mfc = f.read()
    with open(mf, "w") as f:
        mfc = re.sub(r"EXTRAVERSION =\s*\..*\d+\n", "EXTRAVERSION =.{}\\n".format(ver.split(".")[3]), mfc)
        f.write(mfc)

    arch_map = {
            "x86": "x86_64",
            "arm64": "arm64"
            }
    for k in arch_map:
        for fn in ["uvm_defconfig"]:

            cfg = os.path.join(src_tree, "arch/{}/configs/{}".format(k, fn))
            with open(cfg, "r") as f:
                c = f.read()
            with open(cfg, "w") as f:
                c = re.sub("Linux/.*Kernel Configuration", "Linux/{} {} Kernel Configuration".format(arch_map[k], ver), c)
                f.write(c)
