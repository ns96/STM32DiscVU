import javax.sound.sampled.*;
import javax.swing.*;
import javax.swing.border.TitledBorder;
import java.awt.*;
import java.io.*;

public class JDeckTester extends JFrame {

    private JComboBox<Integer> baudRateCombo;
    private JSpinner markFreqSpinner;
    private JSpinner spaceFreqSpinner;
    private JCheckBox invertSignalCheck;
    private JSlider noiseFloorSlider;

    // Encoder (TX) Components
    private JRadioButton txSideA;
    private JRadioButton txSideB;
    private JComboBox<Integer> txDurationCombo;
    private JRadioButton txOutSpeakers;
    private JComboBox<MixerItem> txSpeakerCombo;
    private JRadioButton txOutWav;
    private JButton btnStartTx;
    private JProgressBar txProgress;
    private JLabel txStatusLabel;
    private JTextArea txTerminal;

    // Decoder (RX) Components
    private JTextArea rxTerminal;
    private JTextArea rxDiagTerminal; // Only for diagnostic text
    private JRadioButton rxInMic;
    private JComboBox<MixerItem> rxMicCombo;
    private JRadioButton rxInWav;
    private JButton btnStartRx;
    private JLabel rxStatusLabel;

    // Concurrency state
    private SwingWorker<Void, Void> txWorker;
    private SwingWorker<Void, String> rxWorker;
    private JMinimodem.Config activeRxConfig; // For mid-stream reset
    private TargetDataLine currentMicLine; // Handle to close mic loop early
    private DurationInputStream activeTxStream; // Handle to close tx loop early

    public JDeckTester() {
        super("JDeckTester v.1.0.0 -- 3/13/2026");
        setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        setLayout(new BorderLayout(10, 10));

        // --- TOP: Modem Configuration ---
        JPanel configPanel = new JPanel(new FlowLayout(FlowLayout.LEFT, 15, 5));
        configPanel.setBorder(new TitledBorder("Modem Configuration"));

        configPanel.add(new JLabel("Baud Rate:"));
        baudRateCombo = new JComboBox<>(new Integer[] { 300, 600, 1200 });
        baudRateCombo.setSelectedItem(1200);
        baudRateCombo.setPreferredSize(new Dimension(80, 26)); // Make wider
        configPanel.add(baudRateCombo);

        configPanel.add(new JLabel("Mark Hz:"));
        markFreqSpinner = new JSpinner(new SpinnerNumberModel(1200, 100, 10000, 100));
        configPanel.add(markFreqSpinner);

        configPanel.add(new JLabel("Space Hz:"));
        spaceFreqSpinner = new JSpinner(new SpinnerNumberModel(2200, 100, 10000, 100));
        configPanel.add(spaceFreqSpinner);

        baudRateCombo.addActionListener(e -> {
            int baud = (Integer) baudRateCombo.getSelectedItem();
            if (baud == 300) {
                markFreqSpinner.setValue(1270);
                spaceFreqSpinner.setValue(1070);
            } else { // 600 or 1200
                markFreqSpinner.setValue(1200);
                spaceFreqSpinner.setValue(2200);
            }
        });

        invertSignalCheck = new JCheckBox("Invert Signal");
        configPanel.add(invertSignalCheck);

        configPanel.add(new JLabel("Noise Floor:"));
        noiseFloorSlider = new JSlider(0, 100, 20); // 0.0 to 1.0 mapped to 0-100
        noiseFloorSlider.setPreferredSize(new Dimension(100, 40));
        configPanel.add(noiseFloorSlider);

        add(configPanel, BorderLayout.NORTH);

        // --- CENTER: Split Pane for TX and RX ---
        JSplitPane splitPane = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT);
        splitPane.setResizeWeight(0.5);

        // --- LEFT (TX Panel) ---
        JPanel txPanel = new JPanel(new BorderLayout(5, 5));
        txPanel.setBorder(new TitledBorder("Encoder Settings & Log (TX)"));

        JPanel txTop = new JPanel(new FlowLayout(FlowLayout.LEFT, 5, 0));
        txTop.add(new JLabel("Side: "));
        ButtonGroup sideGroup = new ButtonGroup();
        txSideA = new JRadioButton("Side A", true);
        txSideB = new JRadioButton("Side B");
        sideGroup.add(txSideA);
        sideGroup.add(txSideB);
        txTop.add(txSideA);
        txTop.add(txSideB);

        txTop.add(Box.createHorizontalStrut(15));
        txTop.add(new JLabel("Duration (minutes): "));
        txDurationCombo = new JComboBox<>(new Integer[] { 45, 90, 120, 240 });
        txDurationCombo.setSelectedItem(45);
        txDurationCombo.setPreferredSize(new Dimension(80, 26)); // Make wider
        txTop.add(txDurationCombo);

        txPanel.add(txTop, BorderLayout.NORTH);

        txTerminal = new JTextArea();
        txTerminal.setBackground(Color.BLACK);
        txTerminal.setForeground(Color.ORANGE);
        txTerminal.setFont(new Font("Monospaced", Font.PLAIN, 12));
        txTerminal.setEditable(false);
        JScrollPane txScrollPane = new JScrollPane(txTerminal);
        txPanel.add(txScrollPane, BorderLayout.CENTER);

        JPanel txOutputPanel = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 0));
        txOutputPanel.add(new JLabel("Output: "));
        ButtonGroup txOutGroup = new ButtonGroup();
        txOutSpeakers = new JRadioButton("Speakers", true);
        txSpeakerCombo = new JComboBox<>(getMixers(SourceDataLine.class));
        txOutWav = new JRadioButton("Save to WAV");
        txOutGroup.add(txOutSpeakers);
        txOutGroup.add(txOutWav);
        txOutputPanel.add(txOutSpeakers);
        txOutputPanel.add(txSpeakerCombo);
        txOutputPanel.add(txOutWav);

        JPanel txControls = new JPanel(new BorderLayout(5, 5));
        txControls.add(txOutputPanel, BorderLayout.NORTH);

        btnStartTx = new JButton("RECORD TO TAPE (TX)");
        btnStartTx.setBackground(new Color(200, 50, 50));
        btnStartTx.setForeground(Color.WHITE);
        btnStartTx.setFont(new Font("Dialog", Font.BOLD, 14));
        btnStartTx.addActionListener(e -> toggleTx());
        txControls.add(btnStartTx, BorderLayout.CENTER);

        JPanel txStatusPanel = new JPanel(new BorderLayout());
        txStatusLabel = new JLabel("TX Status: Idle");
        txProgress = new JProgressBar(0, 100);
        txProgress.setStringPainted(true);
        txStatusPanel.add(txStatusLabel, BorderLayout.NORTH);
        txStatusPanel.add(txProgress, BorderLayout.SOUTH);
        txControls.add(txStatusPanel, BorderLayout.SOUTH);

        txPanel.add(txControls, BorderLayout.SOUTH);
        splitPane.setLeftComponent(txPanel);

        // --- RIGHT (RX Panel) ---
        JPanel rxPanel = new JPanel(new BorderLayout(5, 5));
        rxPanel.setBorder(new TitledBorder("Terminal / RX Monitor (Decoder)"));

        // Diagnostic Terminal (Top)
        rxDiagTerminal = new JTextArea(8, 40);
        rxDiagTerminal.setBackground(Color.BLACK);
        rxDiagTerminal.setForeground(Color.LIGHT_GRAY);
        rxDiagTerminal.setFont(new Font("Monospaced", Font.PLAIN, 12));
        rxDiagTerminal.setEditable(false);
        JScrollPane diagScrollPane = new JScrollPane(rxDiagTerminal);

        // Data Terminal (Bottom) - Limit rows
        rxTerminal = new JTextArea(4, 40); // 4 rows visibility
        rxTerminal.setBackground(Color.BLACK);
        rxTerminal.setForeground(Color.GREEN);
        rxTerminal.setFont(new Font("Monospaced", Font.BOLD, 12));
        rxTerminal.setEditable(false);
        JScrollPane dataScrollPane = new JScrollPane(rxTerminal);

        // Split Panes for RX Monitors
        JSplitPane rxMonitorsSplit = new JSplitPane(JSplitPane.VERTICAL_SPLIT, diagScrollPane, dataScrollPane);
        rxMonitorsSplit.setResizeWeight(0.8); // Bias space towards diag terminal
        rxMonitorsSplit.setDividerSize(4);
        rxPanel.add(rxMonitorsSplit, BorderLayout.CENTER);

        JPanel rxControls = new JPanel(new BorderLayout(5, 5));

        JPanel rxInputPanel = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 0));
        rxInputPanel.add(new JLabel("Input: "));
        ButtonGroup rxInGroup = new ButtonGroup();
        rxInMic = new JRadioButton("Microphone", true);
        rxMicCombo = new JComboBox<>(getMixers(TargetDataLine.class));
        rxInWav = new JRadioButton("Read from WAV");
        rxInGroup.add(rxInMic);
        rxInGroup.add(rxInWav);
        rxInputPanel.add(rxInMic);
        rxInputPanel.add(rxMicCombo);
        rxInputPanel.add(rxInWav);
        rxControls.add(rxInputPanel, BorderLayout.NORTH);

        btnStartRx = new JButton("LISTEN TO TAPE (RX)");
        btnStartRx.setBackground(new Color(50, 150, 50));
        btnStartRx.setForeground(Color.WHITE);
        btnStartRx.setFont(new Font("Dialog", Font.BOLD, 14));
        btnStartRx.addActionListener(e -> toggleRx());
        rxControls.add(btnStartRx, BorderLayout.CENTER);

        JPanel rxStatusPanel = new JPanel(new BorderLayout());
        rxStatusLabel = new JLabel("RX Status: Offline");
        rxStatusPanel.add(rxStatusLabel, BorderLayout.NORTH);
        Component rxDummyProgressSpacer = Box.createRigidArea(new Dimension(0, 20)); // Match txProgress height
        rxStatusPanel.add(rxDummyProgressSpacer, BorderLayout.SOUTH);
        rxControls.add(rxStatusPanel, BorderLayout.SOUTH);

        rxPanel.add(rxControls, BorderLayout.SOUTH);
        splitPane.setRightComponent(rxPanel);

        add(splitPane, BorderLayout.CENTER);

        // --- BOTTOM BAR ---
        JPanel bottomBar = new JPanel(new FlowLayout(FlowLayout.RIGHT, 10, 5));

        JButton btnClear = new JButton("Clear Consoles");
        btnClear.addActionListener(e -> {
            rxTerminal.setText("");
            rxDiagTerminal.setText("");
            if (activeRxConfig != null) {
                activeRxConfig.resetDiagnostics = true;
            }
        });

        JButton btnExit = new JButton("Exit");
        btnExit.addActionListener(e -> System.exit(0));

        bottomBar.add(btnClear);
        bottomBar.add(btnExit);
        add(bottomBar, BorderLayout.SOUTH);

        setSize(850, 500);
        setLocationRelativeTo(null);
    }

    /**
     * Builds a JMinimodem.Config object using the current UI state.
     */
    private JMinimodem.Config buildConfig() {
        JMinimodem.Config config = new JMinimodem.Config();
        config.baudRate = (Integer) baudRateCombo.getSelectedItem();
        config.freqMark = (Integer) markFreqSpinner.getValue();
        config.freqSpace = (Integer) spaceFreqSpinner.getValue();
        config.invert = invertSignalCheck.isSelected();
        config.noiseFloor = noiseFloorSlider.getValue() / 100.0;
        config.quiet = false; // Always print diagnostic headers for VisualizerApp logic
        return config;
    }

    // ==========================================================
    // TX Flow (Encoder)
    // ==========================================================
    private void toggleTx() {
        if (txWorker != null && !txWorker.isDone()) {
            if (activeTxStream != null) {
                activeTxStream.earlyStop();
                btnStartTx.setText("STOPPING...");
            }
            return;
        }

        JMinimodem.Config config = buildConfig();
        final int durationSeconds = (Integer) txDurationCombo.getSelectedItem() * 60; // minutes to seconds
        final char side = txSideA.isSelected() ? 'A' : 'B';

        try {
            // Optional File output
            OutputStream fileOut = null;
            if (txOutWav.isSelected()) {
                JFileChooser fc = new JFileChooser();
                if (fc.showSaveDialog(this) == JFileChooser.APPROVE_OPTION) {
                    config.audioFile = fc.getSelectedFile();
                    fileOut = new FileOutputStream(config.audioFile);
                } else {
                    return; // Cancelled
                }
            } else {
                MixerItem selectedSpeaker = (MixerItem) txSpeakerCombo.getSelectedItem();
                if (selectedSpeaker != null && selectedSpeaker.info != null) {
                    AudioFormat format = new AudioFormat(config.sampleRate, 16, 1, true, false);
                    final SourceDataLine targetSpeaker = AudioSystem.getSourceDataLine(format, selectedSpeaker.info);
                    targetSpeaker.open(format);
                    targetSpeaker.start();
                    fileOut = new OutputStream() {
                        @Override
                        public void write(int b) {
                            targetSpeaker.write(new byte[] { (byte) b }, 0, 1);
                        }

                        @Override
                        public void write(byte[] b, int off, int len) {
                            targetSpeaker.write(b, off, len);
                        }

                        @Override
                        public void close() {
                            targetSpeaker.drain();
                            targetSpeaker.close();
                        }
                    };
                }
            }
            final OutputStream targetStream = fileOut;

            btnStartTx.setText("STOP ENCODING (TX)");
            btnStartTx.setBackground(new Color(200, 100, 100));
            txProgress.setMaximum(durationSeconds);
            txStatusLabel.setText("TX Status: Sending DCT to " + (txOutWav.isSelected() ? "File" : "Speakers"));
            txTerminal.setText(""); // Clear terminal

            // Launch background task
            txWorker = new SwingWorker<Void, Void>() {
                @Override
                protected Void doInBackground() throws Exception {
                    // Start the custom stream that handles duration and generation
                    try (DurationInputStream dis = new DurationInputStream((int) config.baudRate, durationSeconds,
                            side);
                            OutputStream os = targetStream) {

                        activeTxStream = dis;
                        dis.setPayloadListener(record -> {
                            SwingUtilities.invokeLater(() -> {
                                txTerminal.append(record);
                                txTerminal.setCaretPosition(txTerminal.getDocument().getLength());
                            });
                        });
                        // JMinimodem blocks here until the stream returns -1
                        JMinimodem.transmit(config, dis, os);
                    } finally {
                        activeTxStream = null;
                    }
                    return null;
                }

                @Override
                protected void done() {
                    btnStartTx.setEnabled(true);
                    btnStartTx.setText("RECORD TO TAPE (TX)");
                    btnStartTx.setBackground(new Color(200, 50, 50));
                    txProgress.setValue(0);
                    txStatusLabel.setText("TX Status: Complete.");
                    try {
                        get();
                    } catch (Exception ex) {
                        ex.printStackTrace();
                    }
                }
            };
            txWorker.execute();

        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, "TX Error: " + ex.getMessage());
            btnStartTx.setEnabled(true);
        }
    }

    // ==========================================================
    // RX Flow (Decoder)
    // ==========================================================
    private void toggleRx() {
        if (rxWorker != null && !rxWorker.isDone()) {
            // Turn OFF Receiver
            if (currentMicLine != null) {
                currentMicLine.stop();
                currentMicLine.close(); // Forcing this closed should break JMinimodem's read loop
            }
            rxWorker.cancel(true);
            btnStartRx.setText("LISTEN TO TAPE (RX)");
            btnStartRx.setBackground(new Color(50, 150, 50));
            rxStatusLabel.setText("RX Status: Offline");
            return;
        }

        JMinimodem.Config config = buildConfig();

        try {
            AudioInputStream ais = null;

            if (rxInWav.isSelected()) {
                JFileChooser fc = new JFileChooser();
                if (fc.showOpenDialog(this) == JFileChooser.APPROVE_OPTION) {
                    ais = AudioSystem.getAudioInputStream(fc.getSelectedFile());
                    config.sampleRate = ais.getFormat().getSampleRate();
                } else {
                    return; // Cancelled
                }
            } else {
                // Mic Input
                AudioFormat req = new AudioFormat(config.sampleRate, 16, 1, true, false);
                DataLine.Info info = new DataLine.Info(TargetDataLine.class, req);
                MixerItem selectedMic = (MixerItem) rxMicCombo.getSelectedItem();

                if (selectedMic != null && selectedMic.info != null) {
                    Mixer mixer = AudioSystem.getMixer(selectedMic.info);
                    currentMicLine = (TargetDataLine) mixer.getLine(info);
                } else {
                    currentMicLine = (TargetDataLine) AudioSystem.getLine(info);
                }

                currentMicLine.open(req);
                currentMicLine.start();
                ais = new AudioInputStream(currentMicLine);
            }

            final AudioInputStream finalAis = ais;
            activeRxConfig = config; // Track for mid-stream reset

            // NEW: Attach diagnostic listener for 1-second polling dashboard (matches C
            // code LCD)
            config.diagListener = diagBlock -> {
                SwingUtilities.invokeLater(() -> {
                    rxDiagTerminal.setText(diagBlock);
                });
            };

            // Re-route System.err (CARRIER / NOCARRIER) back to the main Data Terminal
            // (Bottom)
            PrintStream customErr = new PrintStream(new OutputStream() {
                @Override
                public void write(int b) {
                    SwingUtilities.invokeLater(() -> {
                        rxTerminal.append(String.valueOf((char) b));
                        rxTerminal.setCaretPosition(rxTerminal.getDocument().getLength());
                    });
                }
            }, true);
            PrintStream originalErr = System.err;
            System.setErr(customErr);

            btnStartRx.setText("STOP LISTENING (RX)");
            btnStartRx.setBackground(new Color(200, 50, 50));
            rxStatusLabel.setText("RX Status: Listening...");
            rxTerminal.setText(""); // Clear data terminal
            rxDiagTerminal.setText(""); // Clear diag terminal

            rxWorker = new SwingWorker<Void, String>() {
                @Override
                protected Void doInBackground() throws Exception {
                    // Custom OutputStream to pipe decoded bytes onto the EDT
                    OutputStream textOut = new OutputStream() {
                        @Override
                        public void write(int b) {
                            SwingUtilities.invokeLater(() -> {
                                rxTerminal.append(String.valueOf((char) b));
                                rxTerminal.setCaretPosition(rxTerminal.getDocument().getLength());
                            });
                        }
                    };

                    try {
                        JMinimodem.receive(config, finalAis, textOut);
                    } catch (Exception e) {
                        // Suppress "Line closed" exceptions during manual stop
                    }
                    return null;
                }

                @Override
                protected void done() {
                    // Restore Error Stream
                    System.setErr(originalErr);

                    btnStartRx.setText("LISTEN TO TAPE (RX)");
                    btnStartRx.setBackground(new Color(50, 150, 50));
                    if (currentMicLine != null)
                        currentMicLine.close();
                    activeRxConfig = null;
                }
            };
            rxWorker.execute();

        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, "RX Error: " + ex.getMessage());
        }
    }

    static class MixerItem {
        Mixer.Info info;

        public MixerItem(Mixer.Info info) {
            this.info = info;
        }

        @Override
        public String toString() {
            return info != null ? info.getName() : "Default System Device";
        }
    }

    private MixerItem[] getMixers(Class<? extends DataLine> lineClass) {
        java.util.List<MixerItem> items = new java.util.ArrayList<>();
        items.add(new MixerItem(null)); // Default
        for (Mixer.Info info : AudioSystem.getMixerInfo()) {
            try {
                Mixer m = AudioSystem.getMixer(info);
                Line.Info[] lines = (lineClass == TargetDataLine.class) ? m.getTargetLineInfo() : m.getSourceLineInfo();
                for (Line.Info lineInfo : lines) {
                    if (lineClass.isAssignableFrom(lineInfo.getLineClass())) {
                        items.add(new MixerItem(info));
                        break; // Only add this mixer once
                    }
                }
            } catch (Exception e) {
            }
        }
        return items.toArray(new MixerItem[0]);
    }

    public static void main(String[] args) {
        // Set Look and Feel for better Swing appearances
        try {
            UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
        } catch (Exception e) {
        }

        SwingUtilities.invokeLater(() -> {
            new JDeckTester().setVisible(true);
        });
    }
}
