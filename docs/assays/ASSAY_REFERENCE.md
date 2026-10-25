# SPRING2 Assay Reference: Construction and Rationale

## Overview

This reference was constructed to support fast, high-confidence assay type inference from small FASTQ alignment sketches in SPRING2. Rather than aligning against a full genome or transcriptome, the reference consists of carefully selected, diagnostic genomic subsets that expose assay-specific signals with minimal computational cost.

The design emphasizes:

- robustness across library preparations,
- resistance to sequencing depth and GC bias,
- biological interpretability,
- long-term reproducibility.

## Input resources

- Genome: Human GRCh38 / hg38
- Gene annotation: GENCODE v49 (GTF)
- Coordinate system: UCSC-style chrN contigs
- Sequence extraction tool: bedtools getfasta (strand-aware where appropriate)

## Reference structure

The final reference FASTA (spring2_assay_ref_hg38_gencode49.fa) is a concatenation of four biologically and statistically distinct blocks:

1. RNA exon block (RNA detection)
2. ATAC promoter block (chromatin accessibility detection)
3. Intron / intergenic control block (background normalization)
4. Genome backbone block (fragment geometry and periodicity)

Each block was constructed independently and validated before concatenation.

1. RNA exon block (RNA vs DNA discrimination)

---------------------------------------------
Purpose The RNA exon block robustly distinguishes RNA-derived libraries (bulk RNA-seq, scRNA-seq) from DNA-derived libraries using exonic enrichment and splicing signal.

Gene selection A curated set of ubiquitously expressed housekeeping genes was selected: ACTB, GAPDH, EEF1A1, RPLP0, RPS18, RPL13A, HPRT1, B2M, PABPC1, HNRNPK, MALAT1

Transcript selection

- For protein-coding genes: MANE Select transcripts were used when available.

Construction method

- GENCODE v49 GTF was parsed to extract exon coordinates for selected transcripts.
- Exons were strand-aware, sorted, and concatenated to form spliced exon-only sequences.

Result

- 11 FASTA records, one per transcript, representing continuous spliced mRNA sequences.

1. ATAC promoter block (ATAC vs non-ATAC discrimination)

Purpose

To detect chromatin accessibility libraries (ATAC-seq, scATAC-seq) via sharp enrichment at transcription start sites (TSSs).

Gene selection ACTB, GAPDH, EEF1A1, RPLP0, RPL13A, RPS18, HNRNPA1, HSP90AA1, CFL1, TUBA1B, B2M

TSS determination

- Protein-coding transcripts from GENCODE v49 were used.
- One representative TSS per gene was selected after collapsing transcript isoforms.

Window definition

- Strand-aware ±1,000 bp windows around the TSS.

Result

- 11 FASTA records, each exactly 2,000 bp long.

1. Intron / intergenic control block (background normalization)

Purpose

To provide a neutral background DNA reference for normalization and ratio-based scoring.

Region selection

- 14 intergenic regions, approximately 200 kb each.
- Distributed across chromosomes: chr1, chr3, chr5, chr7, chr12, chr17, chr19.

Result

- 14 FASTA records (~2.8 Mb total).

1. Genome backbone block (fragment geometry and periodicity)

Purpose

To capture fragment-length distributions and nucleosome periodicity for refining DNA, ATAC, and ChIP discrimination.

Region selection

- 7 intergenic regions (400 kb each) across chr1, chr3, chr5, chr7, chr12, chr17, chr19.

Result

- 7 FASTA records (2.8 Mb total).

## Final reference assembly

Blocks were concatenated in the following order:

1. RNA exon block
2. ATAC promoter block
3. Intron / intergenic control block
4. Genome backbone block

Summary statistics

- RNA exons: 11 records
- ATAC promoters: 11 records
- Intron / intergenic controls: 14 records
- Genome backbone: 7 records
- Total: 43 FASTA records

The reference was indexed using samtools faidx for efficient random access.

## Intended use in SPRING2

This reference supports very small alignment sketches (10–50k reads) to enable fast and robust assay inference, including RNA vs DNA and ATAC vs non-ATAC discrimination, and complements barcode-based single-cell detection.

## Versioning

Genome: hg38 (GRCh38) Annotation: GENCODE v49
