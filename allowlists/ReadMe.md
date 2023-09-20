templatemaker.py is a small utility that can autogenerate MSR-Safe allowlist
templates from msr-genie scrapes. 

templatemaker by default expects scrapes to be in ./Intel_MSRs/blr by default
this can be updated by altering the filepath at the top of the file.

templatemaker will display a list of architectures you can select and create an
allowlist for.

Allowlists will be placed in ./templates/ by default.
