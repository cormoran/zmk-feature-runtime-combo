import { render, screen, waitFor } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import { Response } from "../src/proto/cormoran/runtime_combo/runtime_combo";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  call_rpc: jest.fn(),
}));

describe("RPCTestSection Component", () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: /Runtime Combos/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Combo Editor/i })
      ).toBeInTheDocument();
      expect(screen.getByLabelText(/Positions/i)).toBeInTheDocument();
      expect(screen.getByText(/Save Combo/i)).toBeInTheDocument();
    });

    it("should show default input value", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      const input = screen.getByLabelText(/Positions/i) as HTMLInputElement;
      expect(input.value).toBe("0, 1");
    });

    it("should show max combo count from global settings", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      (call_rpc as jest.Mock)
        .mockResolvedValueOnce({
          custom: {
            call: {
              payload: Response.encode(
                Response.create({ listCombos: { combos: [] } })
              ).finish(),
            },
          },
        })
        .mockResolvedValueOnce({
          custom: {
            call: {
              payload: Response.encode(
                Response.create({
                  getGlobalSettings: {
                    settings: {
                      timeoutMs: 75,
                      slowRelease: true,
                      maxCombo: 12,
                    },
                  },
                })
              ).finish(),
            },
          },
        });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByLabelText(/Max combos/i)).toHaveValue(12);
      });
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__runtime_combo" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the runtime combo module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RPCTestSection />);

      expect(container.firstChild).toBeNull();
    });
  });
});
