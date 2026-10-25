# SPRING2 End-to-End Examples

This page collects complete command sequences for common assay pipelines. Each example shows a typical compression flow, optional inspection, and restoration command.

## Conventions

- Replace the example input filenames with your actual files.
- `sample.sp` refers to the SPRING2 archive produced by compression.
- `restored` refers to the output prefix used during decompression.

## Paired-End DNA or WGS

Typical paired-end DNA datasets can use the default `auto` assay mode or an explicit `dna` setting when you want deterministic behavior.

```bash
spring2 -c \
  --R1 tumor_R1.fastq.gz \
  --R2 tumor_R2.fastq.gz \
  --assay dna \
  --threads 16 \
  --memory 16 \
  --note "sample=tumor01 assay=dna" \
  -o tumor01.sp

spring2 --preview -i tumor01.sp

spring2 -d -i tumor01.sp -o tumor01_restored_R1.fastq.gz tumor01_restored_R2.fastq.gz
```

## Bulk RNA-seq

For bulk RNA-seq, use `rna` explicitly if you already know the assay, or keep `auto` if you want SPRING2 to classify the startup sample.

```bash
spring2 -c \
  --R1 rna_R1.fastq.gz \
  --R2 rna_R2.fastq.gz \
  --assay rna \
  --threads 12 \
  --note "project=atlas tissue=liver" \
  -o rna_liver.sp

spring2 --preview -i rna_liver.sp

spring2 -d -i rna_liver.sp -o rna_liver_R1.fastq.gz rna_liver_R2.fastq.gz
```

## ATAC-seq

ATAC workflows benefit from explicit assay selection when you want predictable adapter-aware handling across repeated runs.

```bash
spring2 -c \
  --R1 atac_R1.fastq.gz \
  --R2 atac_R2.fastq.gz \
  --assay atac \
  --threads 12 \
  --note "sample=PBMC atac" \
  -o pbmc_atac.sp

spring2 --preview -i pbmc_atac.sp

spring2 --preview --audit -i pbmc_atac.sp

spring2 -d -i pbmc_atac.sp -o pbmc_atac_R1.fastq.gz pbmc_atac_R2.fastq.gz
```

## Bisulfite Sequencing

Use the bisulfite assay mode when the reads come from bisulfite-converted DNA.

```bash
spring2 -c \
  --R1 bsseq_R1.fastq.gz \
  --R2 bsseq_R2.fastq.gz \
  --assay bisulfite \
  --threads 8 \
  --note "cohort=methylation batch=02" \
  -o methylation_batch02.sp

spring2 --preview -i methylation_batch02.sp

spring2 -d -i methylation_batch02.sp -o methylation_batch02_R1.fastq.gz methylation_batch02_R2.fastq.gz
```

## Single-Cell RNA-seq with I1 Index Reads

When the cellular barcode is stored in I1, pass the grouped index lanes during compression. SPRING2 restores them as grouped outputs during decompression.

```bash
spring2 -c \
  --R1 scrna_R1.fastq.gz \
  --R2 scrna_R2.fastq.gz \
  --I1 scrna_I1.fastq.gz \
  --I2 scrna_I2.fastq.gz \
  --assay sc-rna \
  --threads 16 \
  --note "library=10x sample=donorA" \
  -o donorA_scrna.sp

spring2 --preview -i donorA_scrna.sp

spring2 -d -i donorA_scrna.sp -o donorA_scrna_restored
```

Expected grouped outputs:

- `donorA_scrna_restored.R1`
- `donorA_scrna_restored.R2`
- `donorA_scrna_restored.I1`
- `donorA_scrna_restored.I2`

## Single-Cell RNA-seq with Barcode in R1

When no I1 lane exists and the cellular barcode is part of the R1 prefix, set `--cb-len` to the barcode length.

```bash
spring2 -c \
  --R1 scrna_R1.fastq.gz \
  --R2 scrna_R2.fastq.gz \
  --assay sc-rna \
  --cb-len 16 \
  --threads 16 \
  --note "library=dropseq cb=R1-prefix" \
  -o dropseq_scrna.sp

spring2 --preview -i dropseq_scrna.sp

spring2 -d -i dropseq_scrna.sp -o dropseq_scrna_R1.fastq.gz dropseq_scrna_R2.fastq.gz
```

## Single-Cell ATAC-seq

Single-cell ATAC is typically paired-end and may also benefit from grouped lane handling depending on the upstream sequencer layout.

```bash
spring2 -c \
  --R1 scatac_R1.fastq.gz \
  --R2 scatac_R2.fastq.gz \
  --assay sc-atac \
  --threads 16 \
  --memory 24 \
  --note "sample=multiome01 modality=sc-atac" \
  -o multiome01_scatac.sp

spring2 --preview -i multiome01_scatac.sp

spring2 -d -i multiome01_scatac.sp -o multiome01_scatac_R1.fastq.gz multiome01_scatac_R2.fastq.gz
```

## Single-Cell Bisulfite

Single-cell bisulfite datasets can use explicit assay selection. Unlike sc-RNA, SPRING2 does not strip an R1 cellular-barcode prefix for `sc-bisulfite` (protocols disagree on R1 layout), so `--cb-len` has no effect on that path.

```bash
spring2 -c \
  --R1 scbs_R1.fastq.gz \
  --R2 scbs_R2.fastq.gz \
  --assay sc-bisulfite \
  --threads 12 \
  --note "assay=sc-bisulfite plate=P3" \
  -o plateP3_scbs.sp

spring2 --preview -i plateP3_scbs.sp

spring2 -d -i plateP3_scbs.sp -o plateP3_scbs_R1.fastq.gz plateP3_scbs_R2.fastq.gz
```

## Grouped Read-3 plus Index Pipeline

Use grouped mode when the run includes paired reads plus read-3 and index lanes. Decompression restores all lanes from one archive.

```bash
spring2 -c \
  --R1 run_R1.fastq.gz \
  --R2 run_R2.fastq.gz \
  --R3 run_R3.fastq.gz \
  --I1 run_I1.fastq.gz \
  --I2 run_I2.fastq.gz \
  --assay sc-rna \
  --threads 16 \
  --note "grouped-lanes run=42" \
  -o run42_grouped.sp

spring2 --preview -i run42_grouped.sp

spring2 -d -i run42_grouped.sp -o run42_restored
```

Expected grouped outputs:

- `run42_restored.R1`
- `run42_restored.R2`
- `run42_restored.R3`
- `run42_restored.I1`
- `run42_restored.I2`

## FASTA Example

SPRING2 also supports FASTA input directly.

```bash
spring2 -c --R1 contigs.fasta -o contigs.sp

spring2 --preview -i contigs.sp

spring2 -d -i contigs.sp -o contigs_restored.fasta
```

Use lossy modes only when downstream requirements allow quality modification.

## Verification-Focused Workflow

If the priority is archive validation for transfer or storage, combine compression-time audit and later preview-time audit.

```bash
spring2 -c \
  --R1 archive_R1.fastq.gz \
  --R2 archive_R2.fastq.gz \
  --audit \
  -o archival_copy.sp

spring2 --preview --audit -i archival_copy.sp
```

## See Also

- [Usage Guide](USAGE.md)
- [Compression Guide](COMPRESSION.md)
- [Decompression Guide](DECOMPRESSION.md)
- [Assays and Quality Modes](ASSAYS_AND_QUALITY.md)
- [CLI Reference](CLI_REFERENCE.md)
