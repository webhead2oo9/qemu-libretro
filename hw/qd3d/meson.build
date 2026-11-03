# QD3D Direct3D 9 remoting. host/ and proto/ are synced copies of XPDriver's
# qd3d/ (scripts/qd3d-sync.sh); the executor loads d3d9.dll at run time, so
# there is no link-time Direct3D dependency.
subdir('host')
i386_system_ss.add(files('qd3dpt.c'))
i386_system_ss.add(qd3d_host_ss)
